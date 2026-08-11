#pragma once

#include "moonlight/control/MoonlightControlTypes.h"

#include <filesystem>
#include <optional>
#include <string>

namespace gateway::moonlight {

class MoonlightIdentity {
public:
    explicit MoonlightIdentity(std::filesystem::path storageDirectory = defaultStorageDirectory());

    static std::filesystem::path defaultStorageDirectory();

    const std::filesystem::path& storageDirectory() const;
    const std::filesystem::path& certificatePath() const;
    const std::filesystem::path& privateKeyPath() const;
    const std::string& certificatePem() const;
    const std::string& privateKeyPem() const;
    const std::string& uniqueId() const;

    std::optional<PairedSunshineHost> pairedHost(const std::string& serverUniqueId) const;
    void savePairedHost(const PairedSunshineHost& host);

private:
    void loadOrCreate();
    void createCredentials();
    void validateCredentials() const;

    std::filesystem::path storageDirectory_;
    std::filesystem::path certificatePath_;
    std::filesystem::path privateKeyPath_;
    std::filesystem::path uniqueIdPath_;
    std::filesystem::path hostsPath_;
    std::string certificatePem_;
    std::string privateKeyPem_;
    std::string uniqueId_;
};

} // namespace gateway::moonlight
