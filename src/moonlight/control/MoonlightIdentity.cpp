#include "moonlight/control/MoonlightIdentity.h"

#include <cstdlib>
#include <cctype>
#include <fstream>
#include <iomanip>
#include <memory>
#include <sstream>
#include <stdexcept>

#include <ShlObj.h>
#include <sddl.h>
#include <windows.h>

#include <nlohmann/json.hpp>
#include <openssl/bio.h>
#include <openssl/pem.h>
#include <openssl/rand.h>
#include <openssl/rsa.h>
#include <openssl/x509.h>

namespace gateway::moonlight {
namespace {

using Json = nlohmann::json;

template <typename T, auto FreeFunction>
using OpenSslPointer = std::unique_ptr<T, decltype(FreeFunction)>;

std::string readTextFile(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw std::runtime_error("Unable to read identity file: " + path.string());
    }
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

void writeTextFile(const std::filesystem::path& path, const std::string& contents)
{
    const auto temporaryPath = path.string() + ".tmp";
    {
        std::ofstream output(temporaryPath, std::ios::binary | std::ios::trunc);
        if (!output) {
            throw std::runtime_error("Unable to write identity file: " + path.string());
        }
        output.write(contents.data(), static_cast<std::streamsize>(contents.size()));
        if (!output) {
            throw std::runtime_error("Unable to finish writing identity file: " + path.string());
        }
    }

    std::error_code error;
    std::filesystem::remove(path, error);
    error.clear();
    std::filesystem::rename(temporaryPath, path, error);
    if (error) {
        throw std::runtime_error("Unable to persist identity file: " + path.string());
    }
}

std::string bioContents(BIO* bio)
{
    BUF_MEM* memory = nullptr;
    BIO_get_mem_ptr(bio, &memory);
    if (!memory) {
        throw std::runtime_error("OpenSSL did not return serialized identity data");
    }
    return {memory->data, memory->length};
}

std::string randomUniqueId()
{
    std::uint64_t value = 0;
    if (RAND_bytes(reinterpret_cast<unsigned char*>(&value), sizeof(value)) != 1) {
        throw std::runtime_error("OpenSSL failed to generate a unique client ID");
    }

    std::ostringstream result;
    result << std::hex << std::setfill('0') << std::setw(16) << value;
    return result.str();
}

std::filesystem::path identityPath(const std::filesystem::path& directory,
                                   const char* filename)
{
    return directory / filename;
}

int identityFileCount(const std::filesystem::path& directory)
{
    return static_cast<int>(std::filesystem::exists(identityPath(directory, "client-certificate.pem")))
        + static_cast<int>(std::filesystem::exists(identityPath(directory, "client-private-key.pem")))
        + static_cast<int>(std::filesystem::exists(identityPath(directory, "client-unique-id.txt")));
}

bool isEmptyDirectory(const std::filesystem::path& directory)
{
    std::error_code error;
    const bool empty = std::filesystem::is_empty(directory, error);
    if (error) {
        throw std::runtime_error("Unable to inspect data directory: " + directory.string());
    }
    return empty;
}

std::filesystem::path migrationStagingDirectory(const std::filesystem::path& destination)
{
    return destination.parent_path()
        / ("." + destination.filename().string() + ".migration-staging");
}

void restrictMigrationStagingDirectory(const std::filesystem::path& directory)
{
    // Keep private identity material out of the inherited ProgramData ACL while
    // the migration copy is being assembled. The installer adds the service SID
    // after publishing the directory and before the service can start.
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    constexpr auto Descriptor = L"D:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)";
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
            Descriptor, SDDL_REVISION_1, &descriptor, nullptr)) {
        throw std::runtime_error("Unable to prepare secure Gateway migration ACL: "
                                 + std::to_string(GetLastError()));
    }

    const BOOL applied = SetFileSecurityW(
        directory.c_str(), DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION, descriptor);
    const DWORD error = applied ? ERROR_SUCCESS : GetLastError();
    LocalFree(descriptor);
    if (!applied) {
        throw std::runtime_error("Unable to secure Gateway migration staging directory: "
                                 + std::to_string(error));
    }
}

bool isProgramDataParent(const std::filesystem::path& directory)
{
    std::error_code error;
    const auto programDataParent = MoonlightIdentity::serviceStorageDirectory().parent_path();
    return std::filesystem::equivalent(directory.parent_path(), programDataParent, error)
        && !error;
}

void copyMigrationSource(const std::filesystem::path& source,
                         const std::filesystem::path& staging)
{
    std::error_code error;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(source)) {
        if (entry.is_symlink()) {
            throw std::runtime_error(
                "Refusing to migrate a data directory containing symbolic links");
        }
    }
    for (const auto& entry : std::filesystem::directory_iterator(source)) {
        std::filesystem::copy(entry.path(),
                              staging / entry.path().filename(),
                              std::filesystem::copy_options::recursive,
                              error);
        if (error) {
            throw std::runtime_error(
                "Unable to copy legacy Gateway data: " + error.message());
        }
    }
}

void appendMigrationLog(const std::filesystem::path& directory)
{
    std::ofstream log(directory / "gateway-service.log", std::ios::out | std::ios::app);
    if (!log) {
        throw std::runtime_error("Unable to write the Gateway migration log");
    }
    log << "Gateway data migration completed; legacy source was left untouched.\n";
    if (!log) {
        throw std::runtime_error("Unable to finish the Gateway migration log");
    }
}

} // namespace

MoonlightIdentity::MoonlightIdentity(std::filesystem::path storageDirectory)
    : storageDirectory_(std::move(storageDirectory))
    , certificatePath_(storageDirectory_ / "client-certificate.pem")
    , privateKeyPath_(storageDirectory_ / "client-private-key.pem")
    , uniqueIdPath_(storageDirectory_ / "client-unique-id.txt")
    , hostsPath_(storageDirectory_ / "paired-hosts.json")
    , sunshineHostPath_(storageDirectory_ / "sunshine-host.txt")
{
    loadOrCreate();
}

std::filesystem::path MoonlightIdentity::defaultStorageDirectory()
{
    const char* localAppData = std::getenv("LOCALAPPDATA");
    if (!localAppData || !*localAppData) {
        throw std::runtime_error("LOCALAPPDATA is not available");
    }
    return std::filesystem::path(localAppData) / "MoonlightWebRTC";
}

std::filesystem::path MoonlightIdentity::serviceStorageDirectory()
{
    PWSTR programData = nullptr;
    const HRESULT result = SHGetKnownFolderPath(FOLDERID_ProgramData, 0, nullptr, &programData);
    if (FAILED(result) || !programData) {
        throw std::runtime_error("Unable to resolve the Windows ProgramData known folder");
    }

    const std::filesystem::path directory = std::filesystem::path(programData) / "MoonlightWebRTC";
    CoTaskMemFree(programData);
    return directory;
}

std::filesystem::path MoonlightIdentity::resolveStorageDirectory(
    const std::optional<std::filesystem::path>& explicitDirectory,
    MoonlightDataDirectoryMode mode)
{
    if (explicitDirectory) {
        return *explicitDirectory;
    }
    return mode == MoonlightDataDirectoryMode::Service
        ? serviceStorageDirectory()
        : defaultStorageDirectory();
}

MoonlightIdentityMigrationResult MoonlightIdentity::migrateStorageDirectory(
    const std::filesystem::path& sourceDirectory,
    const std::filesystem::path& destinationDirectory)
{
    if (!std::filesystem::is_directory(sourceDirectory)) {
        throw std::runtime_error("Legacy Gateway data directory does not exist: "
                                 + sourceDirectory.string());
    }

    const bool destinationExists = std::filesystem::exists(destinationDirectory);
    if (destinationExists && !std::filesystem::is_directory(destinationDirectory)) {
        throw std::runtime_error("Gateway data destination is not a directory: "
                                 + destinationDirectory.string());
    }

    if (destinationExists && identityFileCount(destinationDirectory) == 3) {
        validateExistingStorageDirectory(destinationDirectory);
        return MoonlightIdentityMigrationResult::DestinationAuthoritative;
    }
    if (destinationExists && !isEmptyDirectory(destinationDirectory)) {
        throw std::runtime_error(
            "Gateway data destination is populated but has no valid complete identity; refusing to merge");
    }

    validateExistingStorageDirectory(sourceDirectory);

    const std::filesystem::path stagingDirectory = migrationStagingDirectory(destinationDirectory);
    if (std::filesystem::exists(stagingDirectory)) {
        if (!std::filesystem::is_directory(stagingDirectory)) {
            throw std::runtime_error("Gateway migration staging path is not a directory: "
                                     + stagingDirectory.string());
        }
        try {
            validateExistingStorageDirectory(stagingDirectory);
        } catch (...) {
            std::error_code removeError;
            std::filesystem::remove_all(stagingDirectory, removeError);
            if (removeError) {
                throw std::runtime_error(
                    "Gateway migration staging data is invalid and cannot be removed safely: "
                    + removeError.message());
            }
        }
    }

    if (!std::filesystem::exists(stagingDirectory)) {
        std::error_code error;
        std::filesystem::create_directories(stagingDirectory, error);
        if (error) {
            throw std::runtime_error("Unable to create Gateway migration staging directory: "
                                     + error.message());
        }
        try {
            if (isProgramDataParent(stagingDirectory)) {
                restrictMigrationStagingDirectory(stagingDirectory);
            }
            copyMigrationSource(sourceDirectory, stagingDirectory);
            validateExistingStorageDirectory(stagingDirectory);
            appendMigrationLog(stagingDirectory);
        } catch (...) {
            std::error_code removeError;
            std::filesystem::remove_all(stagingDirectory, removeError);
            throw;
        }
    }

    // The staging directory is complete and valid. If a prior attempt stopped after
    // staging, this publishes that same validated copy instead of creating a new identity.
    if (std::filesystem::exists(destinationDirectory)) {
        std::error_code error;
        std::filesystem::remove(destinationDirectory, error);
        if (error) {
            throw std::runtime_error("Unable to replace empty Gateway data destination: "
                                     + error.message());
        }
    }

    std::error_code error;
    std::filesystem::rename(stagingDirectory, destinationDirectory, error);
    if (error) {
        throw std::runtime_error("Unable to publish migrated Gateway data: " + error.message());
    }
    return MoonlightIdentityMigrationResult::Migrated;
}

const std::filesystem::path& MoonlightIdentity::storageDirectory() const
{
    return storageDirectory_;
}

const std::filesystem::path& MoonlightIdentity::certificatePath() const
{
    return certificatePath_;
}

const std::filesystem::path& MoonlightIdentity::privateKeyPath() const
{
    return privateKeyPath_;
}

const std::string& MoonlightIdentity::certificatePem() const
{
    return certificatePem_;
}

const std::string& MoonlightIdentity::privateKeyPem() const
{
    return privateKeyPem_;
}

const std::string& MoonlightIdentity::uniqueId() const
{
    return uniqueId_;
}

void MoonlightIdentity::loadOrCreate()
{
    std::filesystem::create_directories(storageDirectory_);

    const int existingFiles = identityFileCount(storageDirectory_);

    if (existingFiles == 0) {
        createCredentials();
    } else if (existingFiles != 3) {
        throw std::runtime_error(
            "Moonlight identity is incomplete; refusing to regenerate paired credentials");
    } else {
        certificatePem_ = readTextFile(certificatePath_);
        privateKeyPem_ = readTextFile(privateKeyPath_);
        uniqueId_ = readTextFile(uniqueIdPath_);
        while (!uniqueId_.empty()
               && (uniqueId_.back() == '\r' || uniqueId_.back() == '\n')) {
            uniqueId_.pop_back();
        }
    }

    validateCredentials();
    if (uniqueId_.empty()) {
        throw std::runtime_error("Moonlight unique client ID is empty");
    }
}

void MoonlightIdentity::validateExistingStorageDirectory(const std::filesystem::path& directory)
{
    if (!std::filesystem::is_directory(directory) || identityFileCount(directory) != 3) {
        throw std::runtime_error("Gateway data directory does not contain a complete identity: "
                                 + directory.string());
    }

    MoonlightIdentity identity(directory);
    const auto hostsPath = directory / "paired-hosts.json";
    if (std::filesystem::exists(hostsPath)) {
        const Json hosts = Json::parse(readTextFile(hostsPath));
        if (!hosts.is_object()) {
            throw std::runtime_error("Paired Sunshine host data is not a JSON object");
        }
    }
}

void MoonlightIdentity::createCredentials()
{
    OpenSslPointer<X509, X509_free> certificate(X509_new(), X509_free);
    OpenSslPointer<EVP_PKEY, EVP_PKEY_free> privateKey(EVP_RSA_gen(2048), EVP_PKEY_free);
    if (!certificate || !privateKey) {
        throw std::runtime_error("OpenSSL failed to allocate Moonlight credentials");
    }

    if (X509_set_version(certificate.get(), 2) != 1
        || ASN1_INTEGER_set(X509_get_serialNumber(certificate.get()), 0) != 1
        || !X509_gmtime_adj(X509_getm_notBefore(certificate.get()), 0)
        || !X509_gmtime_adj(X509_getm_notAfter(certificate.get()), 60L * 60 * 24 * 365 * 20)
        || X509_set_pubkey(certificate.get(), privateKey.get()) != 1) {
        throw std::runtime_error("OpenSSL failed to initialize Moonlight certificate");
    }

    OpenSslPointer<X509_NAME, X509_NAME_free> name(X509_NAME_new(), X509_NAME_free);
    constexpr auto CommonName = "NVIDIA GameStream Client";
    if (!name
        || X509_NAME_add_entry_by_txt(
               name.get(),
               "CN",
               MBSTRING_ASC,
               reinterpret_cast<const unsigned char*>(CommonName),
               -1,
               -1,
               0)
            != 1
        || X509_set_subject_name(certificate.get(), name.get()) != 1
        || X509_set_issuer_name(certificate.get(), name.get()) != 1
        || X509_sign(certificate.get(), privateKey.get(), EVP_sha256()) <= 0) {
        throw std::runtime_error("OpenSSL failed to sign Moonlight certificate");
    }

    OpenSslPointer<BIO, BIO_free> certificateBio(BIO_new(BIO_s_mem()), BIO_free);
    OpenSslPointer<BIO, BIO_free> privateKeyBio(BIO_new(BIO_s_mem()), BIO_free);
    if (!certificateBio || !privateKeyBio
        || PEM_write_bio_X509(certificateBio.get(), certificate.get()) != 1
        || PEM_write_bio_PrivateKey(
               privateKeyBio.get(), privateKey.get(), nullptr, nullptr, 0, nullptr, nullptr)
            != 1) {
        throw std::runtime_error("OpenSSL failed to serialize Moonlight credentials");
    }

    certificatePem_ = bioContents(certificateBio.get());
    privateKeyPem_ = bioContents(privateKeyBio.get());
    uniqueId_ = randomUniqueId();

    writeTextFile(certificatePath_, certificatePem_);
    writeTextFile(privateKeyPath_, privateKeyPem_);
    writeTextFile(uniqueIdPath_, uniqueId_ + "\n");
}

void MoonlightIdentity::validateCredentials() const
{
    OpenSslPointer<BIO, BIO_free> certificateBio(
        BIO_new_mem_buf(certificatePem_.data(), static_cast<int>(certificatePem_.size())),
        BIO_free);
    OpenSslPointer<BIO, BIO_free> privateKeyBio(
        BIO_new_mem_buf(privateKeyPem_.data(), static_cast<int>(privateKeyPem_.size())),
        BIO_free);
    if (!certificateBio || !privateKeyBio) {
        throw std::runtime_error("OpenSSL failed to read persisted Moonlight credentials");
    }

    OpenSslPointer<X509, X509_free> certificate(
        PEM_read_bio_X509(certificateBio.get(), nullptr, nullptr, nullptr), X509_free);
    OpenSslPointer<EVP_PKEY, EVP_PKEY_free> privateKey(
        PEM_read_bio_PrivateKey(privateKeyBio.get(), nullptr, nullptr, nullptr),
        EVP_PKEY_free);
    if (!certificate || !privateKey
        || X509_check_private_key(certificate.get(), privateKey.get()) != 1) {
        throw std::runtime_error("Persisted Moonlight certificate/private key is invalid");
    }
}

std::optional<PairedSunshineHost> MoonlightIdentity::pairedHost(
    const std::string& serverUniqueId) const
{
    if (!std::filesystem::exists(hostsPath_)) {
        return std::nullopt;
    }

    const Json hosts = Json::parse(readTextFile(hostsPath_));
    const auto iterator = hosts.find(serverUniqueId);
    if (iterator == hosts.end()) {
        return std::nullopt;
    }

    return PairedSunshineHost{
        serverUniqueId,
        iterator->at("hostname").get<std::string>(),
        iterator->at("lastAddress").get<std::string>(),
        iterator->at("httpsPort").get<std::uint16_t>(),
        iterator->at("serverCertificatePem").get<std::string>(),
    };
}

void MoonlightIdentity::savePairedHost(const PairedSunshineHost& host)
{
    Json hosts = Json::object();
    if (std::filesystem::exists(hostsPath_)) {
        hosts = Json::parse(readTextFile(hostsPath_));
    }

    hosts[host.serverUniqueId] = {
        {"hostname", host.hostname},
        {"lastAddress", host.lastAddress},
        {"httpsPort", host.httpsPort},
        {"serverCertificatePem", host.serverCertificatePem},
    };
    writeTextFile(hostsPath_, hosts.dump(2) + "\n");
}

bool MoonlightIdentity::removePairedHost(const std::string& serverUniqueId)
{
    if (!std::filesystem::exists(hostsPath_)) return false;
    Json hosts = Json::parse(readTextFile(hostsPath_));
    if (!hosts.is_object() || !hosts.erase(serverUniqueId)) return false;
    writeTextFile(hostsPath_, hosts.dump(2) + "\n");
    return true;
}

std::optional<std::string> MoonlightIdentity::configuredSunshineHost() const
{
    if (!std::filesystem::exists(sunshineHostPath_)) return std::nullopt;
    std::string host = readTextFile(sunshineHostPath_);
    while (!host.empty() && (host.back() == '\r' || host.back() == '\n')) host.pop_back();
    return host.empty() ? std::nullopt : std::optional<std::string>(std::move(host));
}

void MoonlightIdentity::saveConfiguredSunshineHost(const std::string& host)
{
    if (!isValidSunshineHost(host)) {
        throw std::invalid_argument("Invalid Sunshine host");
    }
    writeTextFile(sunshineHostPath_, host + "\n");
}

bool MoonlightIdentity::isValidSunshineHost(std::string_view host)
{
    if (host.empty() || host.size() > 253 || host.front() == '.' || host.back() == '.') return false;
    std::size_t labelLength = 0;
    bool labelStartsWithHyphen = false;
    for (std::size_t index = 0; index < host.size(); ++index) {
        const unsigned char character = static_cast<unsigned char>(host[index]);
        if (character == '.') {
            if (labelLength == 0 || labelLength > 63 || labelStartsWithHyphen
                || host[index - 1] == '-') return false;
            labelLength = 0;
            labelStartsWithHyphen = false;
        } else if (std::isalnum(character) || character == '-') {
            if (labelLength == 0) labelStartsWithHyphen = character == '-';
            ++labelLength;
        } else {
            return false;
        }
    }
    return labelLength > 0 && labelLength <= 63 && !labelStartsWithHyphen && host.back() != '-';
}

} // namespace gateway::moonlight
