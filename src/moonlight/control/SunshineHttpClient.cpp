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
                                              const std::string& rawQuerySuffix)
{
    return request(true, command, query, timeout, rawQuerySuffix);
}

std::string SunshineHttpClient::request(bool https,
                                        const std::string& command,
                                        const Query& query,
                                        std::chrono::milliseconds timeout,
                                        const std::string& rawQuerySuffix)
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
        applications.push_back({titleNode.text().as_string(), idNode.text().as_int()});
    }
    return applications;
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

} // namespace gateway::moonlight
