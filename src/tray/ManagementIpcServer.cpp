#include "tray/ManagementIpcServer.h"

#include <windows.h>
#include <aclapi.h>
#include <sddl.h>

#include <chrono>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

namespace gateway::tray {
namespace {
constexpr wchar_t PipeName[] = L"\\\\.\\pipe\\MoonlightWebRTCGateway.Management";
class Handle { public: explicit Handle(HANDLE value = INVALID_HANDLE_VALUE) : value_(value) {} ~Handle() { if (value_ != INVALID_HANDLE_VALUE && value_) CloseHandle(value_); } HANDLE get() const { return value_; } private: HANDLE value_; };

std::wstring currentLogonSid()
{
    HANDLE raw = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &raw)) throw std::runtime_error("Unable to inspect tray token");
    Handle token(raw); DWORD size = 0; GetTokenInformation(token.get(), TokenGroups, nullptr, 0, &size);
    std::vector<unsigned char> data(size);
    if (!size || !GetTokenInformation(token.get(), TokenGroups, data.data(), size, &size)) throw std::runtime_error("Unable to inspect tray logon SID");
    const auto* groups = reinterpret_cast<const TOKEN_GROUPS*>(data.data());
    for (DWORD i = 0; i < groups->GroupCount; ++i) if ((groups->Groups[i].Attributes & SE_GROUP_LOGON_ID) == SE_GROUP_LOGON_ID) {
        LPWSTR text = nullptr; if (!ConvertSidToStringSidW(groups->Groups[i].Sid, &text)) break;
        std::wstring sid(text); LocalFree(text); return sid;
    }
    throw std::runtime_error("Tray token has no logon SID");
}

Handle makePipe()
{
    const std::wstring descriptorText = L"D:P(A;;GA;;;LS)(A;;GA;;;" + currentLogonSid() + L")";
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(descriptorText.c_str(), SDDL_REVISION_1, &descriptor, nullptr)) throw std::runtime_error("Unable to create management IPC DACL");
    SECURITY_ATTRIBUTES attributes{sizeof(attributes), descriptor, FALSE};
    const HANDLE pipe = CreateNamedPipeW(PipeName, PIPE_ACCESS_DUPLEX, PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1, managementipc::MaximumMessageBytes + 4, managementipc::MaximumMessageBytes + 4, 0, &attributes);
    LocalFree(descriptor); if (pipe == INVALID_HANDLE_VALUE) throw std::runtime_error("Unable to create management IPC pipe"); return Handle(pipe);
}

void allowLocalServiceObjectAccess(HANDLE object, DWORD accessMask)
{
    DWORD sidSize = SECURITY_MAX_SID_SIZE;
    std::vector<unsigned char> sid(sidSize);
    if (!CreateWellKnownSid(WinLocalServiceSid, nullptr, sid.data(), &sidSize)) {
        throw std::runtime_error("Unable to create LocalService SID for management inspection");
    }
    PACL existingDacl = nullptr;
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    const DWORD readResult = GetSecurityInfo(object, SE_KERNEL_OBJECT,
        DACL_SECURITY_INFORMATION, nullptr, nullptr, &existingDacl, nullptr, &descriptor);
    if (readResult != ERROR_SUCCESS) throw std::runtime_error("Unable to inspect tray process DACL");
    EXPLICIT_ACCESSW access{};
    access.grfAccessPermissions = accessMask;
    access.grfAccessMode = GRANT_ACCESS;
    access.grfInheritance = NO_INHERITANCE;
    access.Trustee.TrusteeForm = TRUSTEE_IS_SID;
    access.Trustee.TrusteeType = TRUSTEE_IS_WELL_KNOWN_GROUP;
    access.Trustee.ptstrName = reinterpret_cast<LPWSTR>(sid.data());
    PACL updatedDacl = nullptr;
    const DWORD aclResult = SetEntriesInAclW(1, &access, existingDacl, &updatedDacl);
    const DWORD writeResult = aclResult == ERROR_SUCCESS
        ? SetSecurityInfo(object, SE_KERNEL_OBJECT, DACL_SECURITY_INFORMATION,
                          nullptr, nullptr, updatedDacl, nullptr)
        : aclResult;
    if (updatedDacl) LocalFree(updatedDacl);
    if (descriptor) LocalFree(descriptor);
    if (writeResult != ERROR_SUCCESS) throw std::runtime_error("Unable to grant LocalService tray inspection");
}

void allowLocalServiceProcessInspection()
{
    allowLocalServiceObjectAccess(GetCurrentProcess(), PROCESS_QUERY_LIMITED_INFORMATION);
    HANDLE rawToken = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), READ_CONTROL | WRITE_DAC, &rawToken)) {
        throw std::runtime_error("Unable to open tray token DACL");
    }
    Handle token(rawToken);
    allowLocalServiceObjectAccess(token.get(), TOKEN_QUERY);
}

std::string win32Error(DWORD error)
{
    return std::system_category().message(static_cast<int>(error)) + " (" + std::to_string(error) + ")";
}

bool verifyLocalService(HANDLE pipe, std::string& failure)
{
    if (!ImpersonateNamedPipeClient(pipe)) { failure = "client impersonation failed: " + win32Error(GetLastError()); return false; }
    HANDLE raw = nullptr; const bool opened = OpenThreadToken(GetCurrentThread(), TOKEN_QUERY, TRUE, &raw) != FALSE; const DWORD tokenError = opened ? ERROR_SUCCESS : GetLastError(); RevertToSelf();
    if (!opened) { failure = "client token query failed: " + win32Error(tokenError); return false; }
    Handle token(raw); DWORD size = 0; GetTokenInformation(token.get(), TokenUser, nullptr, 0, &size); std::vector<unsigned char> data(size);
    if (!size || !GetTokenInformation(token.get(), TokenUser, data.data(), size, &size)) { failure = "client user lookup failed: " + win32Error(GetLastError()); return false; }
    if (!IsWellKnownSid(reinterpret_cast<const TOKEN_USER*>(data.data())->User.Sid, WinLocalServiceSid)) { failure = "client token is not LocalService"; return false; }
    return true;
}

bool readExact(HANDLE pipe, void* data, DWORD size) { DWORD got = 0; return ReadFile(pipe, data, size, &got, nullptr) && got == size; }
bool writeFrame(HANDLE pipe, const std::string& text) { const std::uint32_t size = static_cast<std::uint32_t>(text.size()); DWORD sent = 0; return text.size() <= managementipc::MaximumMessageBytes && WriteFile(pipe, &size, 4, &sent, nullptr) && sent == 4 && WriteFile(pipe, text.data(), size, &sent, nullptr) && sent == size; }
bool readFrame(HANDLE pipe, std::string& text) { std::uint32_t size = 0; if (!readExact(pipe, &size, 4) || size > managementipc::MaximumMessageBytes) return false; text.resize(size); return size == 0 || readExact(pipe, text.data(), size); }
}

ManagementIpcServer::ManagementIpcServer()
{
    allowLocalServiceProcessInspection();
}
ManagementIpcServer::~ManagementIpcServer() { stop(); }
void ManagementIpcServer::start() { if (!thread_.joinable()) { stopRequested_ = false; thread_ = std::thread([this] { run(); }); } }
void ManagementIpcServer::stop() { stopRequested_ = true; condition_.notify_all(); if (auto pipe = static_cast<HANDLE>(activePipe_.load())) CancelIoEx(pipe, nullptr); if (thread_.joinable()) thread_.join(); }

managementipc::Result ManagementIpcServer::execute(const managementipc::Command& command)
{
    std::unique_lock lock(mutex_);
    if (pending_) return {false, "busy", "Another management operation is already in progress"};
    pending_ = command; result_.reset(); condition_.notify_all();
    if (!condition_.wait_for(lock, std::chrono::seconds(12), [this] { return result_.has_value() || stopRequested_.load(); })) {
        pending_.reset();
        return {false, "timeout", lastFailure_.empty() ? "Gateway service did not respond"
                                                        : "Management IPC rejected: " + lastFailure_};
    }
    return result_.value_or(managementipc::Result{false, "unavailable", "Management IPC stopped"});
}

void ManagementIpcServer::run()
{
    while (!stopRequested_) try {
        auto pipe = makePipe(); activePipe_ = pipe.get();
        if (!ConnectNamedPipe(pipe.get(), nullptr) && GetLastError() != ERROR_PIPE_CONNECTED) { std::lock_guard lock(mutex_); lastFailure_ = "pipe connection failed: " + win32Error(GetLastError()); activePipe_ = nullptr; continue; }
        std::string verificationFailure;
        if (!verifyLocalService(pipe.get(), verificationFailure)) { std::lock_guard lock(mutex_); lastFailure_ = verificationFailure; DisconnectNamedPipe(pipe.get()); activePipe_ = nullptr; continue; }
        { std::lock_guard lock(mutex_); lastFailure_.clear(); }
        managementipc::Command command; { std::unique_lock lock(mutex_); condition_.wait(lock, [this] { return pending_.has_value() || stopRequested_.load(); }); if (stopRequested_) break; command = *pending_; }
        const std::string request = managementipc::makeCommand(command); std::string response;
        managementipc::Result result{false, "disconnected", "Gateway service disconnected"};
        if (writeFrame(pipe.get(), request) && readFrame(pipe.get(), response)) { try { result = managementipc::parseResult(response, command.type); } catch (const std::exception&) { result = {false, "invalid-response", "Gateway returned an invalid management response"}; } }
        { std::lock_guard lock(mutex_); pending_.reset(); result_ = std::move(result); } condition_.notify_all(); DisconnectNamedPipe(pipe.get()); activePipe_ = nullptr;
    } catch (...) { activePipe_ = nullptr; Sleep(200); }
}
} // namespace gateway::tray
