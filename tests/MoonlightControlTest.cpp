#include "moonlight/control/MoonlightIdentity.h"
#include "moonlight/control/MoonlightPairing.h"
#include "moonlight/control/MoonlightSession.h"
#include "moonlight/control/SunshineHttpClient.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

void require(bool condition, const std::string& message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}

class TemporaryDirectory {
public:
    TemporaryDirectory()
    {
        path_ = std::filesystem::temp_directory_path()
            / ("moonlight-webrtc-control-test-"
               + std::to_string(
                   std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::create_directories(path_);
    }

    ~TemporaryDirectory()
    {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }

    const std::filesystem::path& path() const
    {
        return path_;
    }

private:
    std::filesystem::path path_;
};

} // namespace

int main()
{
    try {
        const std::string serverInfoXml =
            R"(<?xml version="1.0"?><root status_code="200"><hostname>Sunshine-PC</hostname><appversion>7.1.431.-1</appversion><GfeVersion>3.23.0.74</GfeVersion><uniqueid>host-id</uniqueid><HttpsPort>47984</HttpsPort><ServerCodecModeSupport>2032385</ServerCodecModeSupport><PairStatus>1</PairStatus><currentgame>0</currentgame><state>SUNSHINE_SERVER_FREE</state></root>)";
        const auto serverInfo =
            gateway::moonlight::SunshineHttpClient::parseServerInfoXml(serverInfoXml);
        require(serverInfo.hostname == "Sunshine-PC" && serverInfo.httpsPort == 47984
                    && serverInfo.pairStatus == 1 && serverInfo.currentGame == 0
                    && serverInfo.serverCodecModeSupport == 2032385,
                "/serverinfo XML parsing failed");

        const std::string appListXml =
            R"(<?xml version="1.0"?><root status_code="200"><App><AppTitle>Steam</AppTitle><ID>1</ID></App><App><AppTitle>Desktop</AppTitle><ID>7</ID></App></root>)";
        auto applications =
            gateway::moonlight::SunshineHttpClient::parseAppListXml(appListXml);
        require(applications.size() == 2 && applications[1].title == "Desktop"
                    && applications[1].id == 7,
                "/applist XML parsing failed");
        const auto* desktop = gateway::moonlight::SunshineHttpClient::findApplication(
            applications, "desktop");
        require(desktop && desktop->id == 7, "Case-insensitive Desktop lookup failed");

        const auto settings720 = gateway::defaultStreamSettings();
        const auto settings1080 = gateway::defaultStreamSettings(1920, 1080);
        const auto streamConfiguration =
            gateway::moonlight::MoonlightSession::createStreamConfiguration(settings720);
        require(streamConfiguration.width == 1280 && streamConfiguration.height == 720
                    && streamConfiguration.fps == 60 && streamConfiguration.bitrate == 12000
                    && streamConfiguration.packetSize == 1392
                    && streamConfiguration.streamingRemotely == STREAM_CFG_LOCAL
                    && streamConfiguration.audioConfiguration == AUDIO_CONFIGURATION_STEREO
                    && streamConfiguration.supportedVideoFormats == VIDEO_FORMAT_H264
                    && streamConfiguration.clientRefreshRateX100 == 6000
                    && streamConfiguration.colorSpace == COLORSPACE_REC_709
                    && streamConfiguration.colorRange == COLOR_RANGE_LIMITED,
                "STREAM_CONFIGURATION values are incorrect");

        const auto streamConfiguration1080 =
            gateway::moonlight::MoonlightSession::createStreamConfiguration(settings1080);
        require(streamConfiguration1080.width == 1920
                    && streamConfiguration1080.height == 1080
                    && streamConfiguration1080.fps == 60
                    && streamConfiguration1080.bitrate == 20000
                    && streamConfiguration1080.supportedVideoFormats == VIDEO_FORMAT_H264
                    && streamConfiguration1080.audioConfiguration
                        == AUDIO_CONFIGURATION_STEREO,
                "1080p STREAM_CONFIGURATION values are incorrect");

        const auto launchQuery = gateway::moonlight::SunshineHttpClient::makeLaunchQuery(
            7,
            streamConfiguration,
            gateway::moonlight::MoonlightSession::HostGameOptimizationsEnabled);
        const auto sops = std::find_if(
            launchQuery.begin(), launchQuery.end(), [](const auto& item) {
                return item.first == "sops";
            });
        require(!gateway::moonlight::MoonlightSession::HostGameOptimizationsEnabled
                    && sops != launchQuery.end() && sops->second == "0",
                "Sunshine host game optimizations were not explicitly disabled");

        const auto launchQuery1080 = gateway::moonlight::SunshineHttpClient::makeLaunchQuery(
            7,
            streamConfiguration1080,
            gateway::moonlight::MoonlightSession::HostGameOptimizationsEnabled);
        const auto sops1080 = std::find_if(
            launchQuery1080.begin(), launchQuery1080.end(), [](const auto& item) {
                return item.first == "sops";
            });
        require(sops1080 != launchQuery1080.end() && sops1080->second == "0",
                "Sunshine host game optimizations changed for 1080p");

        const auto* desktopById =
            gateway::moonlight::SunshineHttpClient::findApplicationById(applications, 7);
        require(desktopById && desktopById->title == "Desktop",
                "Sunshine application ID lookup failed");

        gateway::moonlight::ServerInformationStorage serverStorage(
            serverInfo, "127.0.0.1", "rtsp://session-url");
        const auto* serverInformation = serverStorage.get();
        require(std::string(serverInformation->address) == "127.0.0.1"
                    && std::string(serverInformation->serverInfoAppVersion)
                        == "7.1.431.-1"
                    && std::string(serverInformation->serverInfoGfeVersion) == "3.23.0.74"
                    && std::string(serverInformation->rtspSessionUrl)
                        == "rtsp://session-url"
                    && serverInformation->serverCodecModeSupport == 2032385,
                "SERVER_INFORMATION storage/lifetimes are incorrect");

        TemporaryDirectory firstDirectory;
        gateway::moonlight::MoonlightIdentity firstIdentity(firstDirectory.path());
        const std::string firstCertificate = firstIdentity.certificatePem();
        const std::string firstPrivateKey = firstIdentity.privateKeyPem();
        const std::string firstUniqueId = firstIdentity.uniqueId();
        gateway::moonlight::MoonlightIdentity reloadedIdentity(firstDirectory.path());
        require(reloadedIdentity.certificatePem() == firstCertificate
                    && reloadedIdentity.privateKeyPem() == firstPrivateKey
                    && reloadedIdentity.uniqueId() == firstUniqueId,
                "Persistent Moonlight identity was regenerated during reload");

        gateway::moonlight::PairedSunshineHost savedHost{
            "host-id", "Sunshine-PC", "127.0.0.1", 47984, firstCertificate};
        firstIdentity.savePairedHost(savedHost);
        const auto loadedHost = reloadedIdentity.pairedHost("host-id");
        require(loadedHost && loadedHost->serverCertificatePem == firstCertificate
                    && loadedHost->lastAddress == "127.0.0.1",
                "Paired Sunshine host persistence failed");

        TemporaryDirectory secondDirectory;
        gateway::moonlight::MoonlightIdentity secondIdentity(secondDirectory.path());
        require(!gateway::moonlight::SunshineHttpClient::certificatesMatch(
                    firstCertificate, secondIdentity.certificatePem()),
                "Pinned certificate mismatch was not detected");
        require(gateway::moonlight::SunshineHttpClient::certificatesMatch(
                    firstCertificate, reloadedIdentity.certificatePem()),
                "Matching pinned certificate was rejected");

        const std::vector<std::uint8_t> salt(16, 0x42);
        const auto aesKey = gateway::moonlight::MoonlightPairing::deriveAesKey(
            salt, "1234", 7);
        const std::vector<std::uint8_t> plaintext(32, 0x5a);
        const auto ciphertext =
            gateway::moonlight::MoonlightPairing::aesEncrypt(plaintext, aesKey);
        require(ciphertext != plaintext
                    && gateway::moonlight::MoonlightPairing::aesDecrypt(ciphertext, aesKey)
                        == plaintext,
                "Pairing AES helper round-trip failed");

        std::cout << "Moonlight control tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Moonlight control test failed: " << error.what() << '\n';
        return 1;
    }
}
