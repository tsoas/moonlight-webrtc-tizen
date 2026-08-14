#pragma once

#include "moonlight/control/MoonlightControlTypes.h"

#include <filesystem>
#include <optional>
#include <string>

namespace gateway::moonlight {

enum class MoonlightDataDirectoryMode {
    Console,
    Service,
};

enum class MoonlightIdentityMigrationResult {
    Migrated,
    DestinationAuthoritative,
};

class MoonlightIdentity {
public:
    explicit MoonlightIdentity(std::filesystem::path storageDirectory = defaultStorageDirectory());

    static std::filesystem::path defaultStorageDirectory();
    static std::filesystem::path serviceStorageDirectory();
    static std::filesystem::path resolveStorageDirectory(
        const std::optional<std::filesystem::path>& explicitDirectory,
        MoonlightDataDirectoryMode mode);
    static MoonlightIdentityMigrationResult migrateStorageDirectory(
        const std::filesystem::path& sourceDirectory,
        const std::filesystem::path& destinationDirectory);

    const std::filesystem::path& storageDirectory() const;
    const std::filesystem::path& certificatePath() const;
    const std::filesystem::path& privateKeyPath() const;
    const std::string& certificatePem() const;
    const std::string& privateKeyPem() const;
    const std::string& uniqueId() const;

    std::optional<PairedSunshineHost> pairedHost(const std::string& serverUniqueId) const;
    void savePairedHost(const PairedSunshineHost& host);
    bool removePairedHost(const std::string& serverUniqueId);
    std::optional<std::string> configuredSunshineHost() const;
    void saveConfiguredSunshineHost(const std::string& host);
    static bool isValidSunshineHost(std::string_view host);

private:
    static void validateExistingStorageDirectory(const std::filesystem::path& directory);
    void loadOrCreate();
    void createCredentials();
    void validateCredentials() const;

    std::filesystem::path storageDirectory_;
    std::filesystem::path certificatePath_;
    std::filesystem::path privateKeyPath_;
    std::filesystem::path uniqueIdPath_;
    std::filesystem::path hostsPath_;
    std::filesystem::path sunshineHostPath_;
    std::string certificatePem_;
    std::string privateKeyPem_;
    std::string uniqueId_;
};

} // namespace gateway::moonlight
