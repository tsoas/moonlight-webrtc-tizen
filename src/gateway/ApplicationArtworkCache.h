#pragma once

#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace gateway {

struct ApplicationArtwork {
    bool available = false;
    std::string mimeType;
    std::vector<std::uint8_t> bytes;
};

class ApplicationArtworkCache {
public:
    std::optional<ApplicationArtwork> find(const std::string& hostId,
                                           const std::string& appId) const;
    void store(std::string hostId, std::string appId, ApplicationArtwork artwork);

private:
    static std::string key(const std::string& hostId, const std::string& appId);

    std::unordered_map<std::string, ApplicationArtwork> entries_;
    mutable std::mutex mutex_;
};

} // namespace gateway
