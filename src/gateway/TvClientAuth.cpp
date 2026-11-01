#include "gateway/TvClientAuth.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <stdexcept>

#include <windows.h>

#include <nlohmann/json.hpp>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/rand.h>

namespace gateway::tvauth {
namespace {

using Json = nlohmann::json;

constexpr int StoreVersion = 1;
constexpr std::string_view ProofLabel = "moonlight-webrtc-tv-auth-v1:";

std::string toHex(const unsigned char* bytes, std::size_t size)
{
    static constexpr char Digits[] = "0123456789abcdef";
    std::string result(size * 2, '\0');
    for (std::size_t index = 0; index < size; ++index) {
        result[index * 2] = Digits[bytes[index] >> 4];
        result[index * 2 + 1] = Digits[bytes[index] & 0x0F];
    }
    return result;
}

int hexValue(char digit)
{
    if (digit >= '0' && digit <= '9') return digit - '0';
    if (digit >= 'a' && digit <= 'f') return digit - 'a' + 10;
    return -1;
}

std::vector<unsigned char> fromHex(std::string_view value)
{
    if (value.size() % 2 != 0) {
        throw std::invalid_argument("Hex value has an odd length");
    }
    std::vector<unsigned char> bytes(value.size() / 2);
    for (std::size_t index = 0; index < bytes.size(); ++index) {
        const int high = hexValue(value[index * 2]);
        const int low = hexValue(value[index * 2 + 1]);
        if (high < 0 || low < 0) {
            throw std::invalid_argument("Hex value contains a non-hex digit");
        }
        bytes[index] = static_cast<unsigned char>((high << 4) | low);
    }
    return bytes;
}

std::string randomPin()
{
    // Rejection sampling keeps every PIN equally likely.
    for (;;) {
        std::uint16_t value = 0;
        if (RAND_bytes(reinterpret_cast<unsigned char*>(&value), sizeof(value)) != 1) {
            throw std::runtime_error("OpenSSL failed to generate a TV pairing PIN");
        }
        if (value < 60000) {
            std::string pin = std::to_string(value % 10000);
            return std::string(4 - pin.size(), '0') + pin;
        }
    }
}

bool isValidPin(std::string_view pin)
{
    return pin.size() == 4
        && std::all_of(pin.begin(), pin.end(), [](char digit) { return digit >= '0' && digit <= '9'; });
}

} // namespace

std::string randomHex(std::size_t bytes)
{
    std::vector<unsigned char> buffer(bytes);
    if (RAND_bytes(buffer.data(), static_cast<int>(buffer.size())) != 1) {
        throw std::runtime_error("OpenSSL failed to generate random bytes");
    }
    return toHex(buffer.data(), buffer.size());
}

bool isHex(std::string_view value, std::size_t bytes)
{
    return value.size() == bytes * 2
        && std::all_of(value.begin(), value.end(), [](char digit) { return hexValue(digit) >= 0; });
}

std::string hmacSha256Hex(std::string_view keyHex, std::string_view message)
{
    const auto key = fromHex(keyHex);
    std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
    unsigned int digestLength = 0;
    if (!HMAC(EVP_sha256(), key.data(), static_cast<int>(key.size()),
              reinterpret_cast<const unsigned char*>(message.data()), message.size(),
              digest.data(), &digestLength)) {
        throw std::runtime_error("OpenSSL failed to compute HMAC-SHA256");
    }
    return toHex(digest.data(), digestLength);
}

std::string authenticationProof(std::string_view clientSecretHex, std::string_view nonceHex)
{
    return hmacSha256Hex(clientSecretHex, std::string(ProofLabel) + std::string(nonceHex));
}

bool constantTimeEquals(std::string_view left, std::string_view right)
{
    return left.size() == right.size()
        && CRYPTO_memcmp(left.data(), right.data(), left.size()) == 0;
}

std::string sanitizeClientName(std::string_view name)
{
    std::string result;
    for (const char character : name) {
        if (result.size() >= MaximumClientNameLength) break;
        // Keep printable ASCII only; the name is a label shown in the tray.
        if (character >= 0x20 && character < 0x7F) result.push_back(character);
    }
    const auto first = result.find_first_not_of(' ');
    const auto last = result.find_last_not_of(' ');
    result = first == std::string::npos ? std::string() : result.substr(first, last - first + 1);
    return result.empty() ? "Samsung TV" : result;
}

TvClientStore::TvClientStore(std::filesystem::path path)
    : path_(std::move(path))
{
    load();
}

void TvClientStore::load()
{
    std::error_code error;
    if (!std::filesystem::exists(path_, error)) {
        return;
    }
    try {
        std::ifstream input(path_, std::ios::binary);
        const Json value = Json::parse(std::string(std::istreambuf_iterator<char>(input), {}));
        if (value.value("version", 0) != StoreVersion || !value.contains("clients")
            || !value.at("clients").is_array()) {
            return;
        }
        for (const auto& entry : value.at("clients")) {
            TvClient client{entry.value("id", ""), entry.value("name", ""), entry.value("secret", "")};
            if (isHex(client.id, ClientIdBytes) && isHex(client.secret, ClientSecretBytes)) {
                client.name = sanitizeClientName(client.name);
                clients_.push_back(std::move(client));
            }
        }
    } catch (const std::exception&) {
        // An unreadable store trusts nobody; affected TVs pair again.
        clients_.clear();
    }
}

void TvClientStore::save() const
{
    Json clients = Json::array();
    for (const auto& client : clients_) {
        clients.push_back({{"id", client.id}, {"name", client.name}, {"secret", client.secret}});
    }
    const std::string contents = Json{{"version", StoreVersion}, {"clients", std::move(clients)}}.dump(2);

    const auto temporaryPath = std::filesystem::path(path_.wstring() + L".tmp");
    {
        std::ofstream output(temporaryPath, std::ios::binary | std::ios::trunc);
        output.write(contents.data(), static_cast<std::streamsize>(contents.size()));
        if (!output) {
            throw std::runtime_error("Unable to write the paired TV list");
        }
    }
    if (!MoveFileExW(temporaryPath.c_str(), path_.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        std::error_code ignored;
        std::filesystem::remove(temporaryPath, ignored);
        throw std::runtime_error("Unable to persist the paired TV list");
    }
}

std::optional<TvClient> TvClientStore::find(std::string_view id) const
{
    const std::lock_guard lock(mutex_);
    const auto found = std::find_if(clients_.begin(), clients_.end(),
                                    [id](const TvClient& client) { return client.id == id; });
    return found == clients_.end() ? std::nullopt : std::optional<TvClient>(*found);
}

TvClient TvClientStore::add(std::string_view name)
{
    TvClient client{randomHex(ClientIdBytes), sanitizeClientName(name), randomHex(ClientSecretBytes)};
    const std::lock_guard lock(mutex_);
    auto updated = clients_;
    if (updated.size() >= MaximumClients) {
        updated.erase(updated.begin());
    }
    updated.push_back(client);
    std::swap(clients_, updated);
    try {
        save();
    } catch (...) {
        std::swap(clients_, updated);
        throw;
    }
    return client;
}

std::size_t TvClientStore::removeAll()
{
    const std::lock_guard lock(mutex_);
    auto removed = std::move(clients_);
    clients_.clear();
    try {
        save();
    } catch (...) {
        clients_ = std::move(removed);
        throw;
    }
    return removed.size();
}

std::size_t TvClientStore::count() const
{
    const std::lock_guard lock(mutex_);
    return clients_.size();
}

std::string TvPairingWindow::open(Clock::time_point now)
{
    pin_ = randomPin();
    expiresAt_ = now + PairingWindowLifetime;
    failedAttempts_ = 0;
    state_ = PairingState::Waiting;
    return pin_;
}

void TvPairingWindow::expire(Clock::time_point now)
{
    if (state_ == PairingState::Waiting && now >= expiresAt_) {
        state_ = PairingState::Expired;
        pin_.clear();
    }
}

PairingAttempt TvPairingWindow::attempt(std::string_view pin, Clock::time_point now)
{
    expire(now);
    if (state_ != PairingState::Waiting) {
        return PairingAttempt::NotOpen;
    }
    if (isValidPin(pin) && constantTimeEquals(pin, pin_)) {
        state_ = PairingState::Paired;
        pin_.clear();
        return PairingAttempt::Accepted;
    }
    if (++failedAttempts_ >= MaximumPinAttempts) {
        state_ = PairingState::Failed;
        pin_.clear();
        return PairingAttempt::TooManyAttempts;
    }
    return PairingAttempt::IncorrectPin;
}

PairingState TvPairingWindow::state(Clock::time_point now)
{
    expire(now);
    return state_;
}

void TvPairingWindow::close()
{
    if (state_ == PairingState::Waiting) {
        state_ = PairingState::Idle;
    }
    pin_.clear();
}

} // namespace gateway::tvauth
