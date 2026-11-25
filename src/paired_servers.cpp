#include "paired_servers.h"

#include <algorithm>

PairedServers::PairedServers(const std::filesystem::path& data_dir)
    : path_(QString::fromStdString((data_dir / "paired_servers.ini").string())) {}

QList<PairedServer> PairedServers::list() const {
    QSettings file(this->path_, QSettings::IniFormat);
    QList<PairedServer> servers;
    const int count = file.beginReadArray("servers");
    for (int i = 0; i < count; i++) {
        file.setArrayIndex(i);
        servers.append({file.value("id").toString(), file.value("name").toString(),
                        file.value("paired_at").toDateTime()});
    }
    file.endArray();
    return servers;
}

void PairedServers::add(const QString& id, const QString& name, const QDateTime& when) {
    QList<PairedServer> servers = this->list();
    // A server with no id is known by its name
    servers.removeIf([&](const PairedServer& known) {
        return id.isEmpty() ? known.id.isEmpty() && known.name == name : known.id == id;
    });
    servers.prepend({id, name, when});

    QSettings file(this->path_, QSettings::IniFormat);
    file.remove("servers");
    file.beginWriteArray("servers");
    for (int i = 0; i < servers.size(); i++) {
        file.setArrayIndex(i);
        file.setValue("id", servers[i].id);
        file.setValue("name", servers[i].name);
        file.setValue("paired_at", servers[i].paired_at);
    }
    file.endArray();
}
