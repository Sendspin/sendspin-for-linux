#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

/// @brief Blobs as one file per key in a directory. The files hold key material: the directory
/// is 0700 and every file 0600. Not thread-safe; each store is used from one thread.
class BlobStore {
public:
    /// @param dir created on the first save
    explicit BlobStore(std::filesystem::path dir);

    /// @return The bytes stored under key, or nullopt when there are none
    std::optional<std::vector<uint8_t>> load(std::string_view key) const;

    /// @brief Replaces the blob under key atomically and durably
    /// @return false when key is not a plain file name or the write failed
    bool save(std::string_view key, std::span<const uint8_t> data);

private:
    std::filesystem::path dir_;
};
