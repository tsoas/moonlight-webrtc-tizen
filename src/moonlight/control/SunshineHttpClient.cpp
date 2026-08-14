#include "moonlight/control/SunshineHttpClient.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <memory>
#include <sstream>

#include <curl/curl.h>
#include <openssl/pem.h>
#include <openssl/rand.h>
#include <openssl/ssl.h>
#include <pugixml.hpp>

namespace gateway::moonlight {
namespace {

constexpr auto DefaultRequestTimeout = std::chrono::seconds(5);
constexpr auto LaunchTimeout = std::chrono::seconds(120);
constexpr std::size_t MaxArtworkBytes = 8 * 1024 * 1024;

struct CurlGlobalState {
    CurlGlobalState()
    {
        if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) {
            throw std::runtime_error("Unable to initialize libcurl");
        }
    }

    ~CurlGlobalState()
    {
        curl_global_cleanup();
    }
};

struct CurlDeleter {
    void operator()(CURL* handle) const
    {
        curl_easy_cleanup(handle);
    }
};

struct BioDeleter {
    void operator()(BIO* bio) const
    {
        BIO_free(bio);
    }
};

struct X509Deleter {
    void operator()(X509* certificate) const
    {
        X509_free(certificate);
    }
};

struct PinnedCertificateContext {
    std::vector<std::uint8_t> expectedDer;
    bool mismatch = false;
};

CurlGlobalState& curlGlobalState()
{
    static CurlGlobalState state;
    return state;
}

std::vector<std::uint8_t> certificateDer(const std::string& pem)
{
    std::unique_ptr<BIO, BioDeleter> bio(
        BIO_new_mem_buf(pem.data(), static_cast<int>(pem.size())));
    if (!bio) {
        throw std::runtime_error("Unable to allocate Sunshine certificate parser");
    }

    std::unique_ptr<X509, X509Deleter> certificate(
        PEM_read_bio_X509(bio.get(), nullptr, nullptr, nullptr));
    if (!certificate) {
        throw std::runtime_error("Unable to parse Sunshine server certificate");
    }

    const int length = i2d_X509(certificate.get(), nullptr);
    if (length <= 0) {
        throw std::runtime_error("Unable to serialize Sunshine server certificate");
    }

    std::vector<std::uint8_t> der(static_cast<std::size_t>(length));
    unsigned char* output = der.data();
    if (i2d_X509(certificate.get(), &output) != length) {
        throw std::runtime_error("Unable to serialize Sunshine server certificate");
    }
    return der;
}

int verifyPinnedCertificate(X509_STORE_CTX* store, void* argument)
{
    auto* context = static_cast<PinnedCertificateContext*>(argument);
    X509* certificate = X509_STORE_CTX_get0_cert(store);
    if (!context || !certificate) {
        return 0;
    }

    const int length = i2d_X509(certificate, nullptr);
    if (length <= 0 || static_cast<std::size_t>(length) != context->expectedDer.size()) {
        context->mismatch = true;
        X509_STORE_CTX_set_error(store, X509_V_ERR_CERT_REJECTED);
        return 0;
    }

    std::vector<std::uint8_t> actualDer(static_cast<std::size_t>(length));
    unsigned char* output = actualDer.data();
    if (i2d_X509(certificate, &output) != length || actualDer != context->expectedDer) {
        context->mismatch = true;
        X509_STORE_CTX_set_error(store, X509_V_ERR_CERT_REJECTED);
        return 0;
    }

    return 1;
}

CURLcode configurePinnedCertificate(CURL*, void* sslContext, void* argument)
{
    auto* context = static_cast<SSL_CTX*>(sslContext);
    SSL_CTX_set_cert_verify_callback(context, verifyPinnedCertificate, argument);
    return CURLE_OK;
}

std::size_t appendResponse(char* data, std::size_t size, std::size_t count, void* output)
{
    const std::size_t bytes = size * count;
    static_cast<std::string*>(output)->append(data, bytes);
    return bytes;
}

std::string randomHex(std::size_t bytes)
{
    std::vector<unsigned char> value(bytes);
    if (RAND_bytes(value.data(), static_cast<int>(value.size())) != 1) {
        throw std::runtime_error("OpenSSL failed to generate an HTTP request UUID");
    }

    std::ostringstream result;
    result << std::hex << std::setfill('0');
    for (const auto byte : value) {
        result << std::setw(2) << static_cast<unsigned int>(byte);
    }
    return result.str();
}

std::string lowercase(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return value;
}

std::string normalizedContentType(std::string_view contentType)
{
    const auto separator = contentType.find(';');
    std::string normalized(contentType.substr(0, separator));
    const auto first = normalized.find_first_not_of(" \t\r\n");
    const auto last = normalized.find_last_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return {};
    }
    return lowercase(normalized.substr(first, last - first + 1));
}

std::string requiredText(const pugi::xml_node& root, const char* name)
{
    const auto node = root.child(name);
    if (!node) {
        throw std::runtime_error(std::string("Sunshine XML is missing ") + name);
    }
    return node.text().as_string();
}

std::uint16_t parsePort(const std::string& text)
{
    unsigned int value = 0;
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    if (result.ec != std::errc() || result.ptr != text.data() + text.size() || value > 65535) {
        throw std::runtime_error("Invalid Sunshine HTTPS port");
    }
    return static_cast<std::uint16_t>(value);
}

std::string bytesToHex(const char* data, std::size_t size)
{
    std::ostringstream result;
    result << std::hex << std::setfill('0');
    for (std::size_t index = 0; index < size; ++index) {
        result << std::setw(2)
               << static_cast<unsigned int>(static_cast<unsigned char>(data[index]));
    }
    return result.str();
}

} // namespace

SunshineHttpError::SunshineHttpError(int statusCode, const std::string& message)
    : std::runtime_error(message)
    , statusCode_(statusCode)
{
}

int SunshineHttpError::statusCode() const
{
    return statusCode_;
}

SunshineHttpClient::SunshineHttpClient(const MoonlightIdentity& identity,
                                       std::string host,
                                       std::uint16_t httpPort)
    : identity_(identity)
    , host_(std::move(host))
    , httpPort_(httpPort)
{
    (void)curlGlobalState();
}

const std::string& SunshineHttpClient::host() const
{
    return host_;
}

std::uint16_t SunshineHttpClient::httpsPort() const
{
    return httpsPort_;
}

const std::string& SunshineHttpClient::pinnedServerCertificate() const
{
    return pinnedServerCertificatePem_;
}

void SunshineHttpClient::setHttpsPort(std::uint16_t port)
{
    httpsPort_ = port;
}

void SunshineHttpClient::setPinnedServerCertificate(std::string certificatePem)
{
    if (!certificatePem.empty()) {
        (void)certificateDer(certificatePem);
    }
    pinnedServerCertificatePem_ = std::move(certificatePem);
}

std::string SunshineHttpClient::requestHttp(const std::string& command,
                                             const Query& query,
                                             std::chrono::milliseconds timeout)
{
    return request(false, command, query, timeout, {});
}

std::string SunshineHttpClient::requestHttps(const std::string& command,
                                              const Query& query,
                                              std::chrono::milliseconds timeout,
                                              const std::string& rawQuerySuffix,
                                              std::string* responseContentType,
                                              long* responseStatus)
{
    return request(
        true, command, query, timeout, rawQuerySuffix, responseContentType, responseStatus);
}

std::string SunshineHttpClient::request(bool https,
                                        const std::string& command,
                                        const Query& query,
                                        std::chrono::milliseconds timeout,
                                        const std::string& rawQuerySuffix,
                                        std::string* responseContentType,
                                        long* responseStatus)
{
    if (https && (httpsPort_ == 0 || pinnedServerCertificatePem_.empty())) {
        throw std::runtime_error(
            "Refusing Sunshine HTTPS request without a pinned server certificate");
    }

    std::unique_ptr<CURL, CurlDeleter> handle(curl_easy_init());
    if (!handle) {
        throw std::runtime_error("Unable to allocate a libcurl request");
    }

    const auto escape = [&handle](const std::string& value) {
        std::unique_ptr<char, decltype(&curl_free)> encoded(
            curl_easy_escape(handle.get(), value.data(), static_cast<int>(value.size())),
            curl_free);
        if (!encoded) {
            throw std::runtime_error("Unable to URL-encode Sunshine request data");
        }
        return std::string(encoded.get());
    };

    std::ostringstream url;
    url << (https ? "https" : "http") << "://" << host_ << ':'
        << (https ? httpsPort_ : httpPort_) << '/' << command << "?uniqueid="
        << escape(identity_.uniqueId()) << "&uuid=" << randomHex(16);
    for (const auto& [name, value] : query) {
        url << '&' << escape(name) << '=' << escape(value);
    }
    url << rawQuerySuffix;

    std::string response;
    std::array<char, CURL_ERROR_SIZE> errorBuffer{};
    const std::string certificatePath = identity_.certificatePath().string();
    const std::string privateKeyPath = identity_.privateKeyPath().string();
    PinnedCertificateContext pinContext;
    if (https) {
        pinContext.expectedDer = certificateDer(pinnedServerCertificatePem_);
    }

    curl_easy_setopt(handle.get(), CURLOPT_URL, url.str().c_str());
    curl_easy_setopt(handle.get(), CURLOPT_WRITEFUNCTION, appendResponse);
    curl_easy_setopt(handle.get(), CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(handle.get(), CURLOPT_ERRORBUFFER, errorBuffer.data());
    curl_easy_setopt(handle.get(), CURLOPT_CONNECTTIMEOUT_MS, static_cast<long>(timeout.count()));
    curl_easy_setopt(handle.get(), CURLOPT_TIMEOUT_MS, static_cast<long>(timeout.count()));
    curl_easy_setopt(handle.get(), CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(handle.get(), CURLOPT_NOPROXY, "*");
    curl_easy_setopt(handle.get(), CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_1_1);
    curl_easy_setopt(handle.get(), CURLOPT_FRESH_CONNECT, 1L);
    curl_easy_setopt(handle.get(), CURLOPT_FORBID_REUSE, 1L);

    if (https) {
        curl_easy_setopt(handle.get(), CURLOPT_SSLCERTTYPE, "PEM");
        curl_easy_setopt(handle.get(), CURLOPT_SSLCERT, certificatePath.c_str());
        curl_easy_setopt(handle.get(), CURLOPT_SSLKEYTYPE, "PEM");
        curl_easy_setopt(handle.get(), CURLOPT_SSLKEY, privateKeyPath.c_str());
        curl_easy_setopt(handle.get(), CURLOPT_SSL_VERIFYPEER, 1L);
        curl_easy_setopt(handle.get(), CURLOPT_SSL_VERIFYHOST, 0L);
        curl_easy_setopt(handle.get(), CURLOPT_SSL_CTX_FUNCTION, configurePinnedCertificate);
        curl_easy_setopt(handle.get(), CURLOPT_SSL_CTX_DATA, &pinContext);
    }

    const CURLcode result = curl_easy_perform(handle.get());
    if (result != CURLE_OK) {
        if (https && pinContext.mismatch) {
            throw std::runtime_error(
                "Sunshine server certificate mismatch; refusing the connection");
        }
        const std::string detail = errorBuffer[0] ? errorBuffer.data() : curl_easy_strerror(result);
        throw std::runtime_error("Sunshine HTTP request failed: " + detail);
    }

    if (responseContentType) {
        char* contentType = nullptr;
        curl_easy_getinfo(handle.get(), CURLINFO_CONTENT_TYPE, &contentType);
        *responseContentType = contentType ? contentType : "";
    }
    if (responseStatus) {
        curl_easy_getinfo(handle.get(), CURLINFO_RESPONSE_CODE, responseStatus);
    }

    return response;
}

SunshineServerInfo SunshineHttpClient::getServerInfo(bool authenticated,
                                                      std::chrono::milliseconds timeout)
{
    const std::string xml = authenticated
        ? requestHttps("serverinfo", {}, timeout)
        : requestHttp("serverinfo", {}, timeout);
    auto serverInfo = parseServerInfoXml(xml);
    if (serverInfo.httpsPort == 0) {
        serverInfo.httpsPort = 47984;
    }
    setHttpsPort(serverInfo.httpsPort);
    return serverInfo;
}

std::vector<SunshineApp> SunshineHttpClient::getAppList()
{
    const std::string xml = requestHttps("applist", {}, DefaultRequestTimeout);
    return parseAppListXml(xml);
}

SunshineHttpClient::AppArtwork SunshineHttpClient::getAppArtwork(const std::string& appId)
{
    if (appId.empty()) {
        throw std::runtime_error("Sunshine application ID must not be empty");
    }

    std::string contentType;
    long statusCode = 0;
    const std::string bytes = requestHttps(
        "appasset",
        makeAppArtworkQuery(appId),
        DefaultRequestTimeout,
        {},
        &contentType,
        &statusCode);
    if (bytes.empty() || bytes.size() > MaxArtworkBytes) {
        throw std::runtime_error("Sunshine application artwork has an invalid size");
    }

    AppArtwork artwork;
    artwork.requestTarget = appArtworkRequestTarget(appId);
    artwork.httpStatus = statusCode;
    artwork.bytes.assign(bytes.begin(), bytes.end());
    const std::string headerMimeType = normalizedContentType(contentType);
    if (!headerMimeType.empty() && !isSupportedArtworkMimeType(headerMimeType)) {
        throw std::runtime_error("Sunshine application artwork has an unsupported MIME type");
    }
    if (artwork.bytes.size() >= 3 && artwork.bytes[0] == 0xFF && artwork.bytes[1] == 0xD8
        && artwork.bytes[2] == 0xFF) {
        artwork.mimeType = "image/jpeg";
    } else if (artwork.bytes.size() >= 8 && artwork.bytes[0] == 0x89 && artwork.bytes[1] == 0x50
               && artwork.bytes[2] == 0x4E && artwork.bytes[3] == 0x47
               && artwork.bytes[4] == 0x0D && artwork.bytes[5] == 0x0A
               && artwork.bytes[6] == 0x1A && artwork.bytes[7] == 0x0A) {
        artwork.mimeType = "image/png";
    } else if (artwork.bytes.size() >= 12 && artwork.bytes[0] == 'R'
               && artwork.bytes[1] == 'I' && artwork.bytes[2] == 'F'
               && artwork.bytes[3] == 'F' && artwork.bytes[8] == 'W'
               && artwork.bytes[9] == 'E' && artwork.bytes[10] == 'B'
               && artwork.bytes[11] == 'P') {
        artwork.mimeType = "image/webp";
    }
    if (!headerMimeType.empty() && headerMimeType != artwork.mimeType) {
        throw std::runtime_error("Sunshine application artwork MIME type does not match its data");
    }
    if (!isLikelyArtworkImage(artwork.mimeType, artwork.bytes)) {
        throw std::runtime_error("Sunshine application artwork is not a supported image");
    }
    return artwork;
}

std::string SunshineHttpClient::launchOrResume(
    const std::string& verb,
    int appId,
    const STREAM_CONFIGURATION& streamConfiguration,
    bool enableGameOptimizations)
{
    const std::string xml = requestHttps(
        verb,
        makeLaunchQuery(appId, streamConfiguration, enableGameOptimizations),
        LaunchTimeout,
        LiGetLaunchUrlQueryParameters());
    verifyResponseStatus(xml);
    const std::string sessionUrl = xmlValue(xml, "sessionUrl0");
    if (sessionUrl.empty()) {
        throw std::runtime_error("Sunshine launch response did not contain sessionUrl0");
    }
    return sessionUrl;
}

SunshineHttpClient::Query SunshineHttpClient::makeLaunchQuery(
    int appId,
    const STREAM_CONFIGURATION& streamConfiguration,
    bool enableGameOptimizations)
{
    const auto* iv = reinterpret_cast<const unsigned char*>(streamConfiguration.remoteInputAesIv);
    const std::uint32_t remoteInputKeyId = (static_cast<std::uint32_t>(iv[0]) << 24)
        | (static_cast<std::uint32_t>(iv[1]) << 16)
        | (static_cast<std::uint32_t>(iv[2]) << 8) | static_cast<std::uint32_t>(iv[3]);

    Query query{
        {"appid", std::to_string(appId)},
        {"mode",
         std::to_string(streamConfiguration.width) + "x"
             + std::to_string(streamConfiguration.height) + "x"
             + std::to_string(streamConfiguration.fps)},
        {"additionalStates", "1"},
        {"sops", enableGameOptimizations ? "1" : "0"},
        {"rikey",
         bytesToHex(streamConfiguration.remoteInputAesKey,
                    sizeof(streamConfiguration.remoteInputAesKey))},
        {"rikeyid", std::to_string(remoteInputKeyId)},
        {"localAudioPlayMode", "0"},
        {"surroundAudioInfo",
         std::to_string(SURROUNDAUDIOINFO_FROM_AUDIO_CONFIGURATION(
             streamConfiguration.audioConfiguration))},
        {"remoteControllersBitmap", "0"},
        {"gcmap", "0"},
        {"gcpersist", "0"},
    };
    if ((streamConfiguration.supportedVideoFormats & VIDEO_FORMAT_MASK_10BIT) != 0) {
        query.emplace_back("hdrMode", "1");
        query.emplace_back("clientHdrCapVersion", "0");
        query.emplace_back("clientHdrCapSupportedFlagsInUint32", "0");
        query.emplace_back("clientHdrCapMetaDataId", "NV_STATIC_METADATA_TYPE_1");
        query.emplace_back("clientHdrCapDisplayData", "0x0x0x0x0x0x0x0x0x0x0");
    }
    return query;
}

SunshineServerInfo SunshineHttpClient::parseServerInfoXml(const std::string& xml)
{
    verifyResponseStatus(xml);
    pugi::xml_document document;
    const auto result = document.load_string(xml.c_str());
    if (!result) {
        throw std::runtime_error("Invalid Sunshine /serverinfo XML");
    }

    const auto root = document.child("root");
    SunshineServerInfo info;
    info.hostname = requiredText(root, "hostname");
    info.appVersion = requiredText(root, "appversion");
    info.gfeVersion = root.child("GfeVersion").text().as_string();
    info.uniqueId = requiredText(root, "uniqueid");
    info.httpsPort = parsePort(requiredText(root, "HttpsPort"));
    info.serverCodecModeSupport = root.child("ServerCodecModeSupport").text().as_int();
    info.pairStatus = root.child("PairStatus").text().as_int();
    info.currentGame = root.child("currentgame").text().as_int();
    info.state = requiredText(root, "state");
    return info;
}

std::vector<SunshineApp> SunshineHttpClient::parseAppListXml(const std::string& xml)
{
    verifyResponseStatus(xml);
    pugi::xml_document document;
    const auto result = document.load_string(xml.c_str());
    if (!result) {
        throw std::runtime_error("Invalid Sunshine /applist XML");
    }

    std::vector<SunshineApp> applications;
    for (const auto appNode : document.child("root").children("App")) {
        const auto titleNode = appNode.child("AppTitle");
        const auto idNode = appNode.child("ID");
        if (!titleNode || !idNode) {
            throw std::runtime_error("Sunshine /applist contains an incomplete App entry");
        }
        const std::string appId = idNode.text().as_string();
        if (appId.empty()) {
            throw std::runtime_error("Sunshine /applist contains an empty App ID");
        }
        applications.push_back({titleNode.text().as_string(), appId});
    }
    return applications;
}

SunshineHttpClient::Query SunshineHttpClient::makeAppArtworkQuery(const std::string& appId)
{
    if (appId.empty()) {
        throw std::runtime_error("Sunshine application ID must not be empty");
    }
    return {{"appid", appId}, {"AssetType", "2"}, {"AssetIdx", "0"}};
}

std::string SunshineHttpClient::appArtworkRequestTarget(const std::string& appId)
{
    makeAppArtworkQuery(appId);
    return "/appasset?appid=" + appId + "&AssetType=2&AssetIdx=0";
}

const SunshineApp* SunshineHttpClient::findApplication(
    std::vector<SunshineApp>& applications,
    const std::string& name)
{
    const std::string expected = lowercase(name);
    const auto iterator = std::find_if(
        applications.begin(), applications.end(), [&expected](const SunshineApp& application) {
            return lowercase(application.title) == expected;
        });
    return iterator == applications.end() ? nullptr : &*iterator;
}

const SunshineApp* SunshineHttpClient::findApplicationById(
    std::vector<SunshineApp>& applications,
    int id)
{
    const auto iterator = std::find_if(
        applications.begin(), applications.end(), [id](const SunshineApp& application) {
            return application.id == std::to_string(id);
        });
    return iterator == applications.end() ? nullptr : &*iterator;
}

int SunshineHttpClient::numericApplicationId(const std::string& appId)
{
    int value = 0;
    const auto result = std::from_chars(appId.data(), appId.data() + appId.size(), value);
    if (result.ec != std::errc() || result.ptr != appId.data() + appId.size() || value < 0) {
        throw std::runtime_error("Sunshine application ID cannot be used for session launch");
    }
    return value;
}

std::string SunshineHttpClient::xmlValue(const std::string& xml,
                                         const std::string& elementName)
{
    pugi::xml_document document;
    if (!document.load_string(xml.c_str())) {
        throw std::runtime_error("Invalid Sunshine XML response");
    }
    const auto node = document.child("root").child(elementName.c_str());
    return node ? node.text().as_string() : std::string{};
}

std::vector<std::uint8_t> SunshineHttpClient::xmlHexValue(
    const std::string& xml,
    const std::string& elementName)
{
    const std::string value = xmlValue(xml, elementName);
    if (value.size() % 2 != 0) {
        throw std::runtime_error("Sunshine returned an invalid hexadecimal value");
    }

    std::vector<std::uint8_t> output(value.size() / 2);
    for (std::size_t index = 0; index < output.size(); ++index) {
        unsigned int byte = 0;
        const auto begin = value.data() + index * 2;
        const auto result = std::from_chars(begin, begin + 2, byte, 16);
        if (result.ec != std::errc() || result.ptr != begin + 2) {
            throw std::runtime_error("Sunshine returned an invalid hexadecimal value");
        }
        output[index] = static_cast<std::uint8_t>(byte);
    }
    return output;
}

void SunshineHttpClient::verifyResponseStatus(const std::string& xml)
{
    pugi::xml_document document;
    if (!document.load_string(xml.c_str())) {
        throw SunshineHttpError(-1, "Malformed Sunshine XML response");
    }

    const auto root = document.child("root");
    if (!root) {
        throw SunshineHttpError(-1, "Sunshine XML is missing the root element");
    }

    const auto statusAttribute = root.attribute("status_code");
    if (!statusAttribute) {
        throw SunshineHttpError(-1, "Sunshine XML is missing status_code");
    }

    const unsigned long statusUnsigned = std::strtoul(statusAttribute.value(), nullptr, 10);
    const int status = static_cast<int>(statusUnsigned);
    if (status != 200) {
        const std::string message = root.attribute("status_message").as_string();
        throw SunshineHttpError(status,
                                "Sunshine request failed: " + std::to_string(status) + " "
                                    + message);
    }
}

bool SunshineHttpClient::certificatesMatch(const std::string& firstPem,
                                            const std::string& secondPem)
{
    return certificateDer(firstPem) == certificateDer(secondPem);
}

bool SunshineHttpClient::isSupportedArtworkMimeType(std::string_view contentType)
{
    const std::string normalized = normalizedContentType(contentType);
    return normalized == "image/jpeg" || normalized == "image/png" || normalized == "image/webp";
}

bool SunshineHttpClient::isLikelyArtworkImage(
    std::string_view mimeType,
    const std::vector<std::uint8_t>& bytes)
{
    const std::string normalized = normalizedContentType(mimeType);
    if (normalized == "image/jpeg") {
        return bytes.size() >= 3 && bytes[0] == 0xFF && bytes[1] == 0xD8 && bytes[2] == 0xFF;
    }
    if (normalized == "image/png") {
        return bytes.size() >= 8 && bytes[0] == 0x89 && bytes[1] == 0x50 && bytes[2] == 0x4E
            && bytes[3] == 0x47 && bytes[4] == 0x0D && bytes[5] == 0x0A && bytes[6] == 0x1A
            && bytes[7] == 0x0A;
    }
    return normalized == "image/webp" && bytes.size() >= 12 && bytes[0] == 'R'
        && bytes[1] == 'I' && bytes[2] == 'F' && bytes[3] == 'F' && bytes[8] == 'W'
        && bytes[9] == 'E' && bytes[10] == 'B' && bytes[11] == 'P';
}

} // namespace gateway::moonlight
