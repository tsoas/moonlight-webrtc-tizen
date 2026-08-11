#include "moonlight/control/MoonlightIdentity.h"

#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <memory>
#include <sstream>
#include <stdexcept>

#include <nlohmann/json.hpp>
#include <openssl/bio.h>
#include <openssl/pem.h>
#include <openssl/rand.h>
#include <openssl/rsa.h>
#include <openssl/x509.h>

namespace gateway::moonlight {
namespace {

using Json = nlohmann::json;

template <typename T, auto FreeFunction>
using OpenSslPointer = std::unique_ptr<T, decltype(FreeFunction)>;

std::string readTextFile(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw std::runtime_error("Unable to read identity file: " + path.string());
    }
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

void writeTextFile(const std::filesystem::path& path, const std::string& contents)
{
    const auto temporaryPath = path.string() + ".tmp";
    {
        std::ofstream output(temporaryPath, std::ios::binary | std::ios::trunc);
        if (!output) {
            throw std::runtime_error("Unable to write identity file: " + path.string());
        }
        output.write(contents.data(), static_cast<std::streamsize>(contents.size()));
        if (!output) {
            throw std::runtime_error("Unable to finish writing identity file: " + path.string());
        }
    }

    std::error_code error;
    std::filesystem::remove(path, error);
    error.clear();
    std::filesystem::rename(temporaryPath, path, error);
    if (error) {
        throw std::runtime_error("Unable to persist identity file: " + path.string());
    }
}

std::string bioContents(BIO* bio)
{
    BUF_MEM* memory = nullptr;
    BIO_get_mem_ptr(bio, &memory);
    if (!memory) {
        throw std::runtime_error("OpenSSL did not return serialized identity data");
    }
    return {memory->data, memory->length};
}

std::string randomUniqueId()
{
    std::uint64_t value = 0;
    if (RAND_bytes(reinterpret_cast<unsigned char*>(&value), sizeof(value)) != 1) {
        throw std::runtime_error("OpenSSL failed to generate a unique client ID");
    }

    std::ostringstream result;
    result << std::hex << std::setfill('0') << std::setw(16) << value;
    return result.str();
}

} // namespace

MoonlightIdentity::MoonlightIdentity(std::filesystem::path storageDirectory)
    : storageDirectory_(std::move(storageDirectory))
    , certificatePath_(storageDirectory_ / "client-certificate.pem")
    , privateKeyPath_(storageDirectory_ / "client-private-key.pem")
    , uniqueIdPath_(storageDirectory_ / "client-unique-id.txt")
    , hostsPath_(storageDirectory_ / "paired-hosts.json")
{
    loadOrCreate();
}

std::filesystem::path MoonlightIdentity::defaultStorageDirectory()
{
    const char* localAppData = std::getenv("LOCALAPPDATA");
    if (!localAppData || !*localAppData) {
        throw std::runtime_error("LOCALAPPDATA is not available");
    }
    return std::filesystem::path(localAppData) / "MoonlightWebRTC";
}

const std::filesystem::path& MoonlightIdentity::storageDirectory() const
{
    return storageDirectory_;
}

const std::filesystem::path& MoonlightIdentity::certificatePath() const
{
    return certificatePath_;
}

const std::filesystem::path& MoonlightIdentity::privateKeyPath() const
{
    return privateKeyPath_;
}

const std::string& MoonlightIdentity::certificatePem() const
{
    return certificatePem_;
}

const std::string& MoonlightIdentity::privateKeyPem() const
{
    return privateKeyPem_;
}

const std::string& MoonlightIdentity::uniqueId() const
{
    return uniqueId_;
}

void MoonlightIdentity::loadOrCreate()
{
    std::filesystem::create_directories(storageDirectory_);

    const bool certificateExists = std::filesystem::exists(certificatePath_);
    const bool privateKeyExists = std::filesystem::exists(privateKeyPath_);
    const bool uniqueIdExists = std::filesystem::exists(uniqueIdPath_);
    const int existingFiles = static_cast<int>(certificateExists)
        + static_cast<int>(privateKeyExists) + static_cast<int>(uniqueIdExists);

    if (existingFiles == 0) {
        createCredentials();
    } else if (existingFiles != 3) {
        throw std::runtime_error(
            "Moonlight identity is incomplete; refusing to regenerate paired credentials");
    } else {
        certificatePem_ = readTextFile(certificatePath_);
        privateKeyPem_ = readTextFile(privateKeyPath_);
        uniqueId_ = readTextFile(uniqueIdPath_);
        while (!uniqueId_.empty()
               && (uniqueId_.back() == '\r' || uniqueId_.back() == '\n')) {
            uniqueId_.pop_back();
        }
    }

    validateCredentials();
    if (uniqueId_.empty()) {
        throw std::runtime_error("Moonlight unique client ID is empty");
    }
}

void MoonlightIdentity::createCredentials()
{
    OpenSslPointer<X509, X509_free> certificate(X509_new(), X509_free);
    OpenSslPointer<EVP_PKEY, EVP_PKEY_free> privateKey(EVP_RSA_gen(2048), EVP_PKEY_free);
    if (!certificate || !privateKey) {
        throw std::runtime_error("OpenSSL failed to allocate Moonlight credentials");
    }

    if (X509_set_version(certificate.get(), 2) != 1
        || ASN1_INTEGER_set(X509_get_serialNumber(certificate.get()), 0) != 1
        || !X509_gmtime_adj(X509_getm_notBefore(certificate.get()), 0)
        || !X509_gmtime_adj(X509_getm_notAfter(certificate.get()), 60L * 60 * 24 * 365 * 20)
        || X509_set_pubkey(certificate.get(), privateKey.get()) != 1) {
        throw std::runtime_error("OpenSSL failed to initialize Moonlight certificate");
    }

    OpenSslPointer<X509_NAME, X509_NAME_free> name(X509_NAME_new(), X509_NAME_free);
    constexpr auto CommonName = "NVIDIA GameStream Client";
    if (!name
        || X509_NAME_add_entry_by_txt(
               name.get(),
               "CN",
               MBSTRING_ASC,
               reinterpret_cast<const unsigned char*>(CommonName),
               -1,
               -1,
               0)
            != 1
        || X509_set_subject_name(certificate.get(), name.get()) != 1
        || X509_set_issuer_name(certificate.get(), name.get()) != 1
        || X509_sign(certificate.get(), privateKey.get(), EVP_sha256()) <= 0) {
        throw std::runtime_error("OpenSSL failed to sign Moonlight certificate");
    }

    OpenSslPointer<BIO, BIO_free> certificateBio(BIO_new(BIO_s_mem()), BIO_free);
    OpenSslPointer<BIO, BIO_free> privateKeyBio(BIO_new(BIO_s_mem()), BIO_free);
    if (!certificateBio || !privateKeyBio
        || PEM_write_bio_X509(certificateBio.get(), certificate.get()) != 1
        || PEM_write_bio_PrivateKey(
               privateKeyBio.get(), privateKey.get(), nullptr, nullptr, 0, nullptr, nullptr)
            != 1) {
        throw std::runtime_error("OpenSSL failed to serialize Moonlight credentials");
    }

    certificatePem_ = bioContents(certificateBio.get());
    privateKeyPem_ = bioContents(privateKeyBio.get());
    uniqueId_ = randomUniqueId();

    writeTextFile(certificatePath_, certificatePem_);
    writeTextFile(privateKeyPath_, privateKeyPem_);
    writeTextFile(uniqueIdPath_, uniqueId_ + "\n");
}

void MoonlightIdentity::validateCredentials() const
{
    OpenSslPointer<BIO, BIO_free> certificateBio(
        BIO_new_mem_buf(certificatePem_.data(), static_cast<int>(certificatePem_.size())),
        BIO_free);
    OpenSslPointer<BIO, BIO_free> privateKeyBio(
        BIO_new_mem_buf(privateKeyPem_.data(), static_cast<int>(privateKeyPem_.size())),
        BIO_free);
    if (!certificateBio || !privateKeyBio) {
        throw std::runtime_error("OpenSSL failed to read persisted Moonlight credentials");
    }

    OpenSslPointer<X509, X509_free> certificate(
        PEM_read_bio_X509(certificateBio.get(), nullptr, nullptr, nullptr), X509_free);
    OpenSslPointer<EVP_PKEY, EVP_PKEY_free> privateKey(
        PEM_read_bio_PrivateKey(privateKeyBio.get(), nullptr, nullptr, nullptr),
        EVP_PKEY_free);
    if (!certificate || !privateKey
        || X509_check_private_key(certificate.get(), privateKey.get()) != 1) {
        throw std::runtime_error("Persisted Moonlight certificate/private key is invalid");
    }
}

std::optional<PairedSunshineHost> MoonlightIdentity::pairedHost(
    const std::string& serverUniqueId) const
{
    if (!std::filesystem::exists(hostsPath_)) {
        return std::nullopt;
    }

    const Json hosts = Json::parse(readTextFile(hostsPath_));
    const auto iterator = hosts.find(serverUniqueId);
    if (iterator == hosts.end()) {
        return std::nullopt;
    }

    return PairedSunshineHost{
        serverUniqueId,
        iterator->at("hostname").get<std::string>(),
        iterator->at("lastAddress").get<std::string>(),
        iterator->at("httpsPort").get<std::uint16_t>(),
        iterator->at("serverCertificatePem").get<std::string>(),
    };
}

void MoonlightIdentity::savePairedHost(const PairedSunshineHost& host)
{
    Json hosts = Json::object();
    if (std::filesystem::exists(hostsPath_)) {
        hosts = Json::parse(readTextFile(hostsPath_));
    }

    hosts[host.serverUniqueId] = {
        {"hostname", host.hostname},
        {"lastAddress", host.lastAddress},
        {"httpsPort", host.httpsPort},
        {"serverCertificatePem", host.serverCertificatePem},
    };
    writeTextFile(hostsPath_, hosts.dump(2) + "\n");
}

} // namespace gateway::moonlight
