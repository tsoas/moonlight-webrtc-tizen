#pragma once

#include "moonlight/control/MoonlightControlTypes.h"
#include "moonlight/control/MoonlightIdentity.h"

#include <chrono>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <Limelight.h>

namespace gateway::moonlight {

class SunshineHttpError : public std::runtime_error {
public:
    SunshineHttpError(int statusCode, const std::string& message);
    int statusCode() const;

private:
    int statusCode_;
};

class SunshineHttpClient {
public:
    using Query = std::vector<std::pair<std::string, std::string>>;

    struct AppArtwork {
        std::string mimeType;
        std::vector<std::uint8_t> bytes;
        long httpStatus = 0;
        std::string requestTarget;
    };

    SunshineHttpClient(const MoonlightIdentity& identity,
                       std::string host,
                       std::uint16_t httpPort = 47989);

    const std::string& host() const;
    std::uint16_t httpsPort() const;
    const std::string& pinnedServerCertificate() const;

    void setHttpsPort(std::uint16_t port);
    void setPinnedServerCertificate(std::string certificatePem);

    std::string requestHttp(const std::string& command,
                            const Query& query,
                            std::chrono::milliseconds timeout);
    std::string requestHttps(const std::string& command,
                             const Query& query,
                             std::chrono::milliseconds timeout,
                             const std::string& rawQuerySuffix = {},
                             std::string* responseContentType = nullptr,
                             long* responseStatus = nullptr);

    SunshineServerInfo getServerInfo(bool authenticated,
                                     std::chrono::milliseconds timeout);
    std::vector<SunshineApp> getAppList();
    AppArtwork getAppArtwork(const std::string& appId);
    std::string launchOrResume(const std::string& verb,
                               int appId,
                               const STREAM_CONFIGURATION& streamConfiguration,
                               bool enableGameOptimizations);

    static Query makeLaunchQuery(int appId,
                                 const STREAM_CONFIGURATION& streamConfiguration,
                                 bool enableGameOptimizations);

    static SunshineServerInfo parseServerInfoXml(const std::string& xml);
    static std::vector<SunshineApp> parseAppListXml(const std::string& xml);
    static Query makeAppArtworkQuery(const std::string& appId);
    static std::string appArtworkRequestTarget(const std::string& appId);
    static const SunshineApp* findApplication(std::vector<SunshineApp>& applications,
                                               const std::string& name);
    static const SunshineApp* findApplicationById(std::vector<SunshineApp>& applications,
                                                  int id);
    static int numericApplicationId(const std::string& appId);
    static std::string xmlValue(const std::string& xml, const std::string& elementName);
    static std::vector<std::uint8_t> xmlHexValue(const std::string& xml,
                                                 const std::string& elementName);
    static void verifyResponseStatus(const std::string& xml);
    static bool certificatesMatch(const std::string& firstPem, const std::string& secondPem);
    static bool isSupportedArtworkMimeType(std::string_view contentType);
    static bool isLikelyArtworkImage(std::string_view mimeType,
                                     const std::vector<std::uint8_t>& bytes);

private:
    std::string request(bool https,
                        const std::string& command,
                        const Query& query,
                        std::chrono::milliseconds timeout,
                        const std::string& rawQuerySuffix,
                        std::string* responseContentType = nullptr,
                        long* responseStatus = nullptr);

    const MoonlightIdentity& identity_;
    std::string host_;
    std::uint16_t httpPort_;
    std::uint16_t httpsPort_ = 0;
    std::string pinnedServerCertificatePem_;
};

} // namespace gateway::moonlight
