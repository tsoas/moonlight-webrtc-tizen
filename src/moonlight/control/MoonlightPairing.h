#pragma once

#include "moonlight/control/MoonlightIdentity.h"
#include "moonlight/control/SunshineHttpClient.h"

#include <cstdint>
#include <string>
#include <vector>

namespace gateway::moonlight {

class MoonlightPairing {
public:
    enum class Result {
        Paired,
        IncorrectPin,
        AlreadyInProgress,
        Failed,
    };

    struct Outcome {
        Result result = Result::Failed;
        std::string serverCertificatePem;
    };

    MoonlightPairing(const MoonlightIdentity& identity, SunshineHttpClient& httpClient);

    static std::string generatePin();
    static std::vector<std::uint8_t> deriveAesKey(
        const std::vector<std::uint8_t>& salt,
        const std::string& pin,
        int serverMajorVersion);
    static std::vector<std::uint8_t> aesEncrypt(
        const std::vector<std::uint8_t>& plaintext,
        const std::vector<std::uint8_t>& key);
    static std::vector<std::uint8_t> aesDecrypt(
        const std::vector<std::uint8_t>& ciphertext,
        const std::vector<std::uint8_t>& key);

    Outcome pair(const std::string& appVersion, const std::string& pin);

private:
    void cancelPairing() noexcept;

    const MoonlightIdentity& identity_;
    SunshineHttpClient& httpClient_;
};

} // namespace gateway::moonlight
