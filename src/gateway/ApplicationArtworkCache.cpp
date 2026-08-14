#include "gateway/ApplicationArtworkCache.h"

namespace gateway {

std::optional<ApplicationArtwork> ApplicationArtworkCache::find(
    const std::string& hostId,
    const std::string& appId) const
{
    const std::lock_guard lock(mutex_);
    const auto iterator = entries_.find(key(hostId, appId));
    return iterator == entries_.end() ? std::nullopt
                                      : std::optional<ApplicationArtwork>(iterator->second);
}

void ApplicationArtworkCache::store(std::string hostId,
                                    std::string appId,
                                    ApplicationArtwork artwork)
{
    const std::lock_guard lock(mutex_);
    entries_[key(hostId, appId)] = std::move(artwork);
}

std::string ApplicationArtworkCache::key(const std::string& hostId,
                                         const std::string& appId)
{
    return hostId + '\n' + appId;
}

} // namespace gateway
