#include "moonlight/control/MoonlightPairing.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <iomanip>
#include <memory>
#include <sstream>
#include <stdexcept>

#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rand.h>
#include <openssl/x509.h>

namespace gateway::moonlight {
namespace {

constexpr auto RequestTimeout = std::chrono::seconds(5);
constexpr auto PairingWaitTimeout = std::chrono::minutes(10);

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

struct KeyDeleter {
    void operator()(EVP_PKEY* key) const
    {
        EVP_PKEY_free(key);
    }
};

struct CipherContextDeleter {
    void operator()(EVP_CIPHER_CTX* context) const
    {
        EVP_CIPHER_CTX_free(context);
    }
};

struct DigestContextDeleter {
    void operator()(EVP_MD_CTX* context) const
    {
        EVP_MD_CTX_free(context);
    }
};

std::vector<std::uint8_t> randomBytes(std::size_t size)
{
    std::vector<std::uint8_t> output(size);
    if (RAND_bytes(output.data(), static_cast<int>(output.size())) != 1) {
        throw std::runtime_error("OpenSSL failed to generate pairing randomness");
    }
    return output;
}

std::string hex(const std::vector<std::uint8_t>& value)
{
    std::ostringstream result;
    result << std::hex << std::setfill('0');
    for (const auto byte : value) {
        result << std::setw(2) << static_cast<unsigned int>(byte);
    }
    return result.str();
}

std::vector<std::uint8_t> digest(const std::vector<std::uint8_t>& input,
                                 const EVP_MD* algorithm)
{
    std::vector<std::uint8_t> output(static_cast<std::size_t>(EVP_MD_get_size(algorithm)));
    unsigned int outputSize = 0;
    if (EVP_Digest(input.data(), input.size(), output.data(), &outputSize, algorithm, nullptr)
        != 1) {
        throw std::runtime_error("OpenSSL failed to hash pairing data");
    }
    output.resize(outputSize);
    return output;
}

int majorVersion(const std::string& version)
{
    const auto separator = version.find('.');
    const std::string major = version.substr(0, separator);
    if (major.empty()) {
        throw std::runtime_error("Invalid Sunshine appversion");
    }
    return std::stoi(major);
}

const EVP_MD* pairingDigest(int serverMajorVersion)
{
    return serverMajorVersion >= 7 ? EVP_sha256() : EVP_sha1();
}

std::unique_ptr<X509, X509Deleter> loadCertificate(const std::string& pem)
{
    std::unique_ptr<BIO, BioDeleter> bio(
        BIO_new_mem_buf(pem.data(), static_cast<int>(pem.size())));
    if (!bio) {
        throw std::runtime_error("Unable to allocate certificate parser");
    }
    std::unique_ptr<X509, X509Deleter> certificate(
        PEM_read_bio_X509(bio.get(), nullptr, nullptr, nullptr));
    if (!certificate) {
        throw std::runtime_error("Unable to parse pairing certificate");
    }
    return certificate;
}

std::unique_ptr<EVP_PKEY, KeyDeleter> loadPrivateKey(const std::string& pem)
{
    std::unique_ptr<BIO, BioDeleter> bio(
        BIO_new_mem_buf(pem.data(), static_cast<int>(pem.size())));
    if (!bio) {
        throw std::runtime_error("Unable to allocate private key parser");
    }
    std::unique_ptr<EVP_PKEY, KeyDeleter> key(
        PEM_read_bio_PrivateKey(bio.get(), nullptr, nullptr, nullptr));
    if (!key) {
        throw std::runtime_error("Unable to parse Moonlight private key");
    }
    return key;
}

std::vector<std::uint8_t> certificateSignature(X509* certificate)
{
    const ASN1_BIT_STRING* signature = nullptr;
    X509_get0_signature(&signature, nullptr, certificate);
    if (!signature) {
        throw std::runtime_error("Pairing certificate has no signature");
    }
    const auto* data = ASN1_STRING_get0_data(signature);
    const int length = ASN1_STRING_length(signature);
    return {data, data + length};
}

bool verifySignature(const std::vector<std::uint8_t>& data,
                     const std::vector<std::uint8_t>& signature,
                     X509* certificate)
{
    std::unique_ptr<EVP_PKEY, KeyDeleter> publicKey(X509_get_pubkey(certificate));
    std::unique_ptr<EVP_MD_CTX, DigestContextDeleter> context(EVP_MD_CTX_new());
    if (!publicKey || !context
        || EVP_DigestVerifyInit(
               context.get(), nullptr, EVP_sha256(), nullptr, publicKey.get())
            != 1
        || EVP_DigestVerifyUpdate(context.get(), data.data(), data.size()) != 1) {
        throw std::runtime_error("OpenSSL failed to initialize pairing verification");
    }
    return EVP_DigestVerifyFinal(
               context.get(), signature.data(), signature.size())
        == 1;
}

std::vector<std::uint8_t> sign(const std::vector<std::uint8_t>& data, EVP_PKEY* privateKey)
{
    std::unique_ptr<EVP_MD_CTX, DigestContextDeleter> context(EVP_MD_CTX_new());
    if (!context
        || EVP_DigestSignInit(context.get(), nullptr, EVP_sha256(), nullptr, privateKey) != 1
        || EVP_DigestSignUpdate(context.get(), data.data(), data.size()) != 1) {
        throw std::runtime_error("OpenSSL failed to initialize pairing signature");
    }

    std::size_t signatureSize = 0;
    if (EVP_DigestSignFinal(context.get(), nullptr, &signatureSize) != 1) {
        throw std::runtime_error("OpenSSL failed to size pairing signature");
    }
    std::vector<std::uint8_t> signature(signatureSize);
    if (EVP_DigestSignFinal(context.get(), signature.data(), &signatureSize) != 1) {
        throw std::runtime_error("OpenSSL failed to sign pairing data");
    }
    signature.resize(signatureSize);
    return signature;
}

std::string vectorToString(const std::vector<std::uint8_t>& value)
{
    return {reinterpret_cast<const char*>(value.data()), value.size()};
}

} // namespace

MoonlightPairing::MoonlightPairing(const MoonlightIdentity& identity,
                                   SunshineHttpClient& httpClient)
    : identity_(identity)
    , httpClient_(httpClient)
{
}

std::string MoonlightPairing::generatePin()
{
    std::uint16_t value = 0;
    do {
        if (RAND_bytes(reinterpret_cast<unsigned char*>(&value), sizeof(value)) != 1) {
            throw std::runtime_error("OpenSSL failed to generate the pairing PIN");
        }
    } while (value >= 60000);

    std::ostringstream pin;
    pin << std::setfill('0') << std::setw(4) << (value % 10000);
    return pin.str();
}

std::vector<std::uint8_t> MoonlightPairing::deriveAesKey(
    const std::vector<std::uint8_t>& salt,
    const std::string& pin,
    int serverMajorVersion)
{
    std::vector<std::uint8_t> saltedPin = salt;
    saltedPin.insert(saltedPin.end(), pin.begin(), pin.end());
    auto key = digest(saltedPin, pairingDigest(serverMajorVersion));
    key.resize(16);
    return key;
}

std::vector<std::uint8_t> MoonlightPairing::aesEncrypt(
    const std::vector<std::uint8_t>& plaintext,
    const std::vector<std::uint8_t>& key)
{
    if (key.size() != 16 || plaintext.size() % 16 != 0) {
        throw std::invalid_argument("Pairing AES input must use a 16-byte key and full blocks");
    }

    std::unique_ptr<EVP_CIPHER_CTX, CipherContextDeleter> context(EVP_CIPHER_CTX_new());
    std::vector<std::uint8_t> ciphertext(plaintext.size());
    int outputLength = 0;
    if (!context
        || EVP_EncryptInit_ex(context.get(), EVP_aes_128_ecb(), nullptr, key.data(), nullptr)
            != 1
        || EVP_CIPHER_CTX_set_padding(context.get(), 0) != 1
        || EVP_EncryptUpdate(context.get(),
                             ciphertext.data(),
                             &outputLength,
                             plaintext.data(),
                             static_cast<int>(plaintext.size()))
            != 1
        || outputLength != static_cast<int>(plaintext.size())) {
        throw std::runtime_error("OpenSSL failed to encrypt pairing data");
    }
    return ciphertext;
}

std::vector<std::uint8_t> MoonlightPairing::aesDecrypt(
    const std::vector<std::uint8_t>& ciphertext,
    const std::vector<std::uint8_t>& key)
{
    if (key.size() != 16 || ciphertext.size() % 16 != 0) {
        throw std::invalid_argument("Pairing AES input must use a 16-byte key and full blocks");
    }

    std::unique_ptr<EVP_CIPHER_CTX, CipherContextDeleter> context(EVP_CIPHER_CTX_new());
    std::vector<std::uint8_t> plaintext(ciphertext.size());
    int outputLength = 0;
    if (!context
        || EVP_DecryptInit_ex(context.get(), EVP_aes_128_ecb(), nullptr, key.data(), nullptr)
            != 1
        || EVP_CIPHER_CTX_set_padding(context.get(), 0) != 1
        || EVP_DecryptUpdate(context.get(),
                             plaintext.data(),
                             &outputLength,
                             ciphertext.data(),
                             static_cast<int>(ciphertext.size()))
            != 1
        || outputLength != static_cast<int>(ciphertext.size())) {
        throw std::runtime_error("OpenSSL failed to decrypt pairing data");
    }
    return plaintext;
}

MoonlightPairing::Outcome MoonlightPairing::pair(const std::string& appVersion,
                                                  const std::string& pin)
{
    try {
        const int serverMajorVersion = majorVersion(appVersion);
        const int hashLength = EVP_MD_get_size(pairingDigest(serverMajorVersion));
        const auto salt = randomBytes(16);
        const auto aesKey = deriveAesKey(salt, pin, serverMajorVersion);

        const std::vector<std::uint8_t> clientCertificate(
            identity_.certificatePem().begin(), identity_.certificatePem().end());
        const std::string certificateResponse = httpClient_.requestHttp(
            "pair",
            {{"devicename", "roth"},
             {"updateState", "1"},
             {"phrase", "getservercert"},
             {"salt", hex(salt)},
             {"clientcert", hex(clientCertificate)}},
            PairingWaitTimeout);
        SunshineHttpClient::verifyResponseStatus(certificateResponse);
        if (SunshineHttpClient::xmlValue(certificateResponse, "paired") != "1") {
            cancelPairing();
            return {Result::Failed, {}};
        }

        const auto serverCertificateBytes = SunshineHttpClient::xmlHexValue(
            certificateResponse, "plaincert");
        if (serverCertificateBytes.empty()) {
            cancelPairing();
            return {Result::AlreadyInProgress, {}};
        }
        const std::string serverCertificatePem = vectorToString(serverCertificateBytes);
        auto serverCertificate = loadCertificate(serverCertificatePem);
        auto clientCertificateObject = loadCertificate(identity_.certificatePem());
        auto privateKey = loadPrivateKey(identity_.privateKeyPem());
        httpClient_.setPinnedServerCertificate(serverCertificatePem);

        const auto randomChallenge = randomBytes(16);
        const auto encryptedChallenge = aesEncrypt(randomChallenge, aesKey);
        const std::string challengeXml = httpClient_.requestHttp(
            "pair",
            {{"devicename", "roth"},
             {"updateState", "1"},
             {"clientchallenge", hex(encryptedChallenge)}},
            RequestTimeout);
        SunshineHttpClient::verifyResponseStatus(challengeXml);
        if (SunshineHttpClient::xmlValue(challengeXml, "paired") != "1") {
            cancelPairing();
            return {Result::Failed, {}};
        }

        const auto encryptedChallengeResponse = SunshineHttpClient::xmlHexValue(
            challengeXml, "challengeresponse");
        const auto challengeResponseData = aesDecrypt(encryptedChallengeResponse, aesKey);
        if (challengeResponseData.size() < static_cast<std::size_t>(hashLength + 16)) {
            cancelPairing();
            return {Result::Failed, {}};
        }

        const std::vector<std::uint8_t> serverResponse(
            challengeResponseData.begin(), challengeResponseData.begin() + hashLength);
        const std::vector<std::uint8_t> serverChallenge(
            challengeResponseData.begin() + hashLength,
            challengeResponseData.begin() + hashLength + 16);
        const auto clientSecret = randomBytes(16);

        std::vector<std::uint8_t> challengeResponse = serverChallenge;
        const auto clientCertificateSignature = certificateSignature(clientCertificateObject.get());
        challengeResponse.insert(challengeResponse.end(),
                                 clientCertificateSignature.begin(),
                                 clientCertificateSignature.end());
        challengeResponse.insert(
            challengeResponse.end(), clientSecret.begin(), clientSecret.end());
        auto paddedHash = digest(challengeResponse, pairingDigest(serverMajorVersion));
        paddedHash.resize(32);

        const std::string responseXml = httpClient_.requestHttp(
            "pair",
            {{"devicename", "roth"},
             {"updateState", "1"},
             {"serverchallengeresp", hex(aesEncrypt(paddedHash, aesKey))}},
            RequestTimeout);
        SunshineHttpClient::verifyResponseStatus(responseXml);
        if (SunshineHttpClient::xmlValue(responseXml, "paired") != "1") {
            cancelPairing();
            return {Result::Failed, {}};
        }

        const auto pairingSecret = SunshineHttpClient::xmlHexValue(
            responseXml, "pairingsecret");
        if (pairingSecret.size() <= 16) {
            cancelPairing();
            return {Result::Failed, {}};
        }
        const std::vector<std::uint8_t> serverSecret(
            pairingSecret.begin(), pairingSecret.begin() + 16);
        const std::vector<std::uint8_t> serverSignature(
            pairingSecret.begin() + 16, pairingSecret.end());
        if (!verifySignature(serverSecret, serverSignature, serverCertificate.get())) {
            cancelPairing();
            throw std::runtime_error("Sunshine pairing signature verification failed");
        }

        std::vector<std::uint8_t> expectedResponseData = randomChallenge;
        const auto serverCertificateSignature = certificateSignature(serverCertificate.get());
        expectedResponseData.insert(expectedResponseData.end(),
                                    serverCertificateSignature.begin(),
                                    serverCertificateSignature.end());
        expectedResponseData.insert(
            expectedResponseData.end(), serverSecret.begin(), serverSecret.end());
        if (digest(expectedResponseData, pairingDigest(serverMajorVersion)) != serverResponse) {
            cancelPairing();
            return {Result::IncorrectPin, {}};
        }

        std::vector<std::uint8_t> clientPairingSecret = clientSecret;
        const auto clientSignature = sign(clientSecret, privateKey.get());
        clientPairingSecret.insert(
            clientPairingSecret.end(), clientSignature.begin(), clientSignature.end());
        const std::string secretXml = httpClient_.requestHttp(
            "pair",
            {{"devicename", "roth"},
             {"updateState", "1"},
             {"clientpairingsecret", hex(clientPairingSecret)}},
            RequestTimeout);
        SunshineHttpClient::verifyResponseStatus(secretXml);
        if (SunshineHttpClient::xmlValue(secretXml, "paired") != "1") {
            cancelPairing();
            return {Result::Failed, {}};
        }

        const std::string finalXml = httpClient_.requestHttps(
            "pair",
            {{"devicename", "roth"},
             {"updateState", "1"},
             {"phrase", "pairchallenge"}},
            RequestTimeout);
        SunshineHttpClient::verifyResponseStatus(finalXml);
        if (SunshineHttpClient::xmlValue(finalXml, "paired") != "1") {
            cancelPairing();
            return {Result::Failed, {}};
        }

        return {Result::Paired, serverCertificatePem};
    } catch (...) {
        cancelPairing();
        throw;
    }
}

void MoonlightPairing::cancelPairing() noexcept
{
    try {
        (void)httpClient_.requestHttp("unpair", {}, RequestTimeout);
    } catch (...) {
    }
}


} // namespace gateway::moonlight
