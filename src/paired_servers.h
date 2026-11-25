#pragma once

#include <QDateTime>
#include <QList>
#include <QSettings>
#include <QString>

#include <filesystem>

/// @brief A server this PC paired with
struct PairedServer {
    QString id;
    QString name;
    QDateTime paired_at;

    bool operator==(const PairedServer&) const = default;
};

/// @brief The app's own record of the servers that paired with this PC, in
/// `<data_dir>/paired_servers.ini`. The library keeps the pairing records and lists none, so
/// this can go stale: a server that unpaired or was evicted stays (docs/SPEC.md, "Depends on").
class PairedServers {
public:
    explicit PairedServers(const std::filesystem::path& data_dir);

    /// @brief The latest pairing first
    QList<PairedServer> list() const;

    /// @brief Records a pairing; one with a known id takes the place of the earlier one
    void add(const QString& id, const QString& name, const QDateTime& when);

private:
    QString path_;
};
