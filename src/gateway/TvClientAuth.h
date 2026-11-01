#pragma once

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace gateway::tvauth {

inline constexpr std::size_t ClientIdBytes = 16;
inline constexpr std::size_t ClientSecretBytes = 32;
inline constexpr std::size_t NonceBytes = 32;
inline constexpr std::size_t MaximumClients = 32;
inline constexpr std::size_t MaximumClientNameLength = 64;
inline constexpr int MaximumPinAttempts = 3;
inline constexpr std::chrono::seconds PairingWindowLifetime{120};

std::string randomHex(std::size_t bytes);
bool isHex(std::string_view value, std::size_t bytes);
std::string hmacSha256Hex(std::string_view keyHex, std::string_view message);
// The TV proves it holds its secret without sending it: HMAC-SHA256 keyed with the
// secret over a label and the Gateway's single-use nonce. The label keeps the proof
// from being reusable as an HMAC over the bare nonce in any other context.
std::string authenticationProof(std::string_view clientSecretHex, std::string_view nonceHex);
bool constantTimeEquals(std::string_view left, std::string_view right);
std::string sanitizeClientName(std::string_view name);

struct TvClient {
    std::string id;
    std::string name;
    std::string secret;
};

// Paired TVs, persisted beside the Gateway identity in the service-owned data directory.
// The Gateway must recompute each proof, so secrets are stored as issued; the directory
// ACL protects them like the Moonlight private key next to them.
class TvClientStore {
public:
    explicit TvClientStore(std::filesystem::path path);

    std::optional<TvClient> find(std::string_view id) const;
    TvClient add(std::string_view name);
    std::size_t removeAll();
    std::size_t count() const;

private:
    void load();
    void save() const;

    std::filesystem::path path_;
    mutable std::mutex mutex_;
    std::vector<TvClient> clients_;
};

enum class PairingAttempt { Accepted, IncorrectPin, NotOpen, TooManyAttempts };
enum class PairingState { Idle, Waiting, Paired, Expired, Failed };

// A short, explicitly opened window in which one TV may pair with the PIN shown on the PC.
// It closes on success, on expiry, or after MaximumPinAttempts wrong PINs, so a device on
// the network cannot guess its way through the 10,000 possible PINs.
class TvPairingWindow {
public:
    using Clock = std::chrono::steady_clock;

    std::string open(Clock::time_point now);
    PairingAttempt attempt(std::string_view pin, Clock::time_point now);
    PairingState state(Clock::time_point now);
    void close();

private:
    void expire(Clock::time_point now);

    std::string pin_;
    Clock::time_point expiresAt_{};
    int failedAttempts_ = 0;
    PairingState state_ = PairingState::Idle;
};

} // namespace gateway::tvauth
