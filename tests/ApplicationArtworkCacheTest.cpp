#include "gateway/ApplicationArtworkCache.h"

#include <iostream>
#include <stdexcept>
#include <string>

namespace {

void require(bool condition, const std::string& message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}

} // namespace

int main()
{
    try {
        gateway::ApplicationArtworkCache cache;
        require(!cache.find("sunshine-a", "7"), "Artwork cache unexpectedly contains an entry");

        cache.store("sunshine-a", "7", {true, "image/jpeg", {0xFF, 0xD8, 0xFF}});
        const auto artwork = cache.find("sunshine-a", "7");
        require(artwork && artwork->available && artwork->mimeType == "image/jpeg"
                    && artwork->bytes.size() == 3,
                "Artwork cache did not retain the fetched artwork");
        require(!cache.find("sunshine-b", "7"),
                "Artwork cache must be scoped to the Sunshine host identity");

        cache.store("sunshine-a", "4294967295", {true, "image/png", {0x89, 0x50}});
        cache.store("sunshine-a", "4294967296", {true, "image/webp", {'R', 'I'}});
        const auto firstLargeId = cache.find("sunshine-a", "4294967295");
        const auto secondLargeId = cache.find("sunshine-a", "4294967296");
        require(firstLargeId && secondLargeId && firstLargeId->mimeType == "image/png"
                    && secondLargeId->mimeType == "image/webp",
                "Artwork cache must keep distinct Sunshine application IDs separate");

        cache.store("sunshine-a", "9", {});
        const auto missing = cache.find("sunshine-a", "9");
        require(missing && !missing->available,
                "Artwork cache must retain a known missing artwork result");

        std::cout << "Application artwork cache tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Application artwork cache test failed: " << error.what() << '\n';
        return 1;
    }
}
