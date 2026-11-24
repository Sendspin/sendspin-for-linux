#pragma once

#include "blob_store.h"

#include <sendspin/client.h>
#include <sendspin/server/server.h>

#include <filesystem>

/// @brief `$XDG_DATA_HOME/sendspin`, the root of everything the app persists
std::filesystem::path data_dir();

/// @brief The server's key and client records, in `<data_dir>/server`
class ServerPersistence final : public sendspin::server::PersistenceProvider {
public:
    explicit ServerPersistence(const std::filesystem::path& data_dir);

    std::optional<std::vector<uint8_t>> load_blob(const std::string& key) override;
    bool save_blob(const std::string& key, const uint8_t* data, size_t len) override;

private:
    BlobStore store_;
};

/// @brief The client's keypair and pairing records, in `<data_dir>/client`
class ClientPersistence final : public sendspin::SendspinPersistenceProvider {
public:
    explicit ClientPersistence(const std::filesystem::path& data_dir);

    std::optional<size_t> read_blob(std::string_view key, std::span<uint8_t> out) override;
    bool write_blob(std::string_view key, std::span<const uint8_t> data) override;

private:
    BlobStore store_;
};
