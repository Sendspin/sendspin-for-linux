#include "persistence.h"

#include <QStandardPaths>

#include <algorithm>

std::filesystem::path data_dir() {
    const QString base = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation);
    return std::filesystem::path(base.toStdString()) / "sendspin";
}

ServerPersistence::ServerPersistence(const std::filesystem::path& data_dir)
    : store_(data_dir / "server") {}

std::optional<std::vector<uint8_t>> ServerPersistence::load_blob(const std::string& key) {
    return this->store_.load(key);
}

bool ServerPersistence::save_blob(const std::string& key, const uint8_t* data, size_t len) {
    return this->store_.save(key, {data, len});
}

ClientPersistence::ClientPersistence(const std::filesystem::path& data_dir)
    : store_(data_dir / "client") {}

std::optional<size_t> ClientPersistence::read_blob(std::string_view key, std::span<uint8_t> out) {
    const auto blob = this->store_.load(key);
    if (!blob || blob->size() > out.size()) {
        return std::nullopt;
    }
    std::ranges::copy(*blob, out.begin());
    return blob->size();
}

bool ClientPersistence::write_blob(std::string_view key, std::span<const uint8_t> data) {
    return this->store_.save(key, data);
}
