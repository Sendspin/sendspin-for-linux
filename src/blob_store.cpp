#include "blob_store.h"

#include <QLoggingCategory>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <string>
#include <system_error>
#include <utility>

// Log the key's name and errno only, never blob contents: they are secrets.
Q_LOGGING_CATEGORY(lc_store, "sendspin.store")

namespace {

// Keys become file names, so nothing that could leave the directory or hide a file
bool valid_key(std::string_view key) {
    return !key.empty() && std::ranges::all_of(key, [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
               c == '_' || c == '-';
    });
}

bool write_all(int fd, std::span<const uint8_t> data) {
    while (!data.empty()) {
        const ssize_t n = ::write(fd, data.data(), data.size());
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        data = data.subspan(static_cast<size_t>(n));
    }
    return true;
}

void sync_dir(const std::filesystem::path& dir) {
    const int fd = ::open(dir.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd >= 0) {
        ::fsync(fd);
        ::close(fd);
    }
}

}  // namespace

BlobStore::BlobStore(std::filesystem::path dir) : dir_(std::move(dir)) {}

std::optional<std::vector<uint8_t>> BlobStore::load(std::string_view key) const {
    if (!valid_key(key)) {
        return std::nullopt;
    }
    const int fd = ::open((this->dir_ / key).c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        if (errno != ENOENT) {
            qCWarning(lc_store, "cannot open %.*s: %s", static_cast<int>(key.size()), key.data(),
                      std::strerror(errno));
        }
        return std::nullopt;
    }
    std::vector<uint8_t> blob;
    uint8_t chunk[4096];
    ssize_t n = 0;
    while ((n = ::read(fd, chunk, sizeof(chunk))) != 0) {
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            qCWarning(lc_store, "cannot read %.*s: %s", static_cast<int>(key.size()), key.data(),
                      std::strerror(errno));
            ::close(fd);
            return std::nullopt;
        }
        blob.insert(blob.end(), chunk, chunk + n);
    }
    ::close(fd);
    return blob;
}

bool BlobStore::save(std::string_view key, std::span<const uint8_t> data) {
    if (!valid_key(key)) {
        qCWarning(lc_store, "rejected a key that is not a plain file name");
        return false;
    }

    std::error_code ec;
    std::filesystem::create_directories(this->dir_.parent_path(), ec);
    if (::mkdir(this->dir_.c_str(), 0700) != 0 && errno != EEXIST) {
        qCWarning(lc_store, "cannot create %s: %s", this->dir_.c_str(), std::strerror(errno));
        return false;
    }

    // A dot-prefixed name can never be a key, so the temporary cannot shadow a blob
    const std::filesystem::path path = this->dir_ / key;
    const std::filesystem::path tmp = this->dir_ / ("." + std::string(key) + ".tmp");
    const int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0) {
        qCWarning(lc_store, "cannot create %.*s: %s", static_cast<int>(key.size()), key.data(),
                  std::strerror(errno));
        return false;
    }
    // A temporary left by a crash keeps its old mode through O_CREAT
    bool ok = ::fchmod(fd, 0600) == 0 && write_all(fd, data) && ::fsync(fd) == 0;
    ok = ::close(fd) == 0 && ok;
    ok = ok && ::rename(tmp.c_str(), path.c_str()) == 0;
    if (!ok) {
        qCWarning(lc_store, "cannot write %.*s: %s", static_cast<int>(key.size()), key.data(),
                  std::strerror(errno));
        ::unlink(tmp.c_str());
        return false;
    }
    sync_dir(this->dir_);
    return true;
}
