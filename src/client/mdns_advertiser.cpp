#include "mdns_advertiser.h"

#include <QLoggingCategory>
#include <QNetworkInterface>
#include <QtEndian>

#include <avahi-common/alternative.h>
#include <avahi-common/error.h>
#include <avahi-common/malloc.h>
#include <avahi-common/strlst.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <utility>

Q_LOGGING_CATEGORY(lc_mdns, "sendspin.mdns")

namespace {

constexpr auto REFRESH_INTERVAL = std::chrono::seconds(5);
constexpr int MAX_LABEL_BYTES = 63;

// Bridges and virtual NICs of containers and VMs, and VPNs that are not point-to-point. They
// are Ethernet to the kernel, so only the name tells. A plain `br0` may bridge the LAN and stays.
constexpr std::array VIRTUAL_PREFIXES = {"docker", "br-",     "veth",      "virbr",   "vnet",
                                         "vmnet",  "vboxnet", "lxc",       "lxd",     "podman",
                                         "cni",    "flannel", "cali",      "tun",     "tap",
                                         "wg",     "zt",      "tailscale", "nordlynx"};

bool is_virtual(const QString& name) {
    return std::ranges::any_of(VIRTUAL_PREFIXES, [&](const char* prefix) {
        return name.startsWith(QLatin1String(prefix));
    });
}

bool is_announced(const NetInterface::Address& address) {
    const QHostAddress& ip = address.ip;
    if (ip.isNull() || ip.isLoopback() || ip.isLinkLocal() || ip.isMulticast()) {
        return false;
    }
    // The client library listens on IPv4 alone: https://github.com/Sendspin/sendspin-cpp/issues/285
    return ip.protocol() == QAbstractSocket::IPv4Protocol;
}

/// Cuts text to what fits a DNS label
QByteArray label(QString text) {
    while (text.toUtf8().size() > MAX_LABEL_BYTES) {
        text.chop(1);
    }
    return text.toUtf8();
}

bool to_avahi(const QHostAddress& ip, AvahiAddress& out) {
    if (ip.protocol() == QAbstractSocket::IPv4Protocol) {
        out.proto = AVAHI_PROTO_INET;
        out.data.ipv4.address = qToBigEndian(ip.toIPv4Address());
        return true;
    }
    if (ip.protocol() == QAbstractSocket::IPv6Protocol) {
        out.proto = AVAHI_PROTO_INET6;
        const Q_IPV6ADDR bytes = ip.toIPv6Address();
        std::copy_n(bytes.c, 16, out.data.ipv6.address);
        return true;
    }
    return false;
}

}  // namespace

QList<NetInterface> system_interfaces() {
    QList<NetInterface> result;
    for (const QNetworkInterface& interface : QNetworkInterface::allInterfaces()) {
        const auto flags = interface.flags();
        NetInterface entry;
        entry.name = interface.name();
        entry.index = interface.index();
        entry.running =
            flags.testFlag(QNetworkInterface::IsUp) && flags.testFlag(QNetworkInterface::IsRunning);
        entry.loopback = flags.testFlag(QNetworkInterface::IsLoopBack);
        entry.point_to_point = flags.testFlag(QNetworkInterface::IsPointToPoint);
        for (const QNetworkAddressEntry& address : interface.addressEntries()) {
            // Qt marks temporary and deprecated addresses, and link-local ones, ineligible
            entry.addresses.append(
                {address.ip(), address.dnsEligibility() == QNetworkAddressEntry::DnsIneligible});
        }
        result.append(entry);
    }
    return result;
}

QList<Announcement> announcements(const QList<NetInterface>& interfaces) {
    QList<Announcement> result;
    for (const NetInterface& interface : interfaces) {
        if (!interface.running || interface.loopback || interface.point_to_point ||
            is_virtual(interface.name)) {
            continue;
        }
        Announcement announcement{interface.index, {}};
        for (const NetInterface::Address& address : interface.addresses) {
            if (is_announced(address)) {
                // The zone of a scoped address is not part of what is published
                QHostAddress ip = address.ip;
                ip.setScopeId({});
                announcement.addresses.append(ip);
            }
        }
        if (!announcement.addresses.isEmpty()) {
            result.append(announcement);
        }
    }
    return result;
}

MdnsAdvertiser::MdnsAdvertiser(QObject* parent) : QObject(parent) {
    this->refresh_timer_.setInterval(REFRESH_INTERVAL);
    connect(&this->refresh_timer_, &QTimer::timeout, this, &MdnsAdvertiser::refresh_);
}

MdnsAdvertiser::~MdnsAdvertiser() {
    this->stop();
}

void MdnsAdvertiser::start(const QString& name, const QByteArray& host_label, uint16_t port) {
    this->stop();
    this->txt_name_ = name.toUtf8();
    this->name_ = label(name);
    this->host_name_ = host_label + ".local";
    this->port_ = port;
    this->announcements_ = announcements(system_interfaces());

    this->poll_ = avahi_threaded_poll_new();
    if (this->poll_ == nullptr) {
        qCWarning(lc_mdns, "no avahi poll; not advertising");
        return;
    }
    this->connect_();
    avahi_threaded_poll_start(this->poll_);
    this->refresh_timer_.start();
}

void MdnsAdvertiser::stop() {
    this->refresh_timer_.stop();
    if (this->poll_ == nullptr) {
        return;
    }
    avahi_threaded_poll_stop(this->poll_);
    // Freeing the client frees its group, which withdraws the records
    if (this->client_ != nullptr) {
        avahi_client_free(this->client_);
    }
    avahi_threaded_poll_free(this->poll_);
    this->client_ = nullptr;
    this->group_ = nullptr;
    this->poll_ = nullptr;
}

/// Before the poll's thread runs, or under its lock
void MdnsAdvertiser::connect_() {
    if (this->client_ != nullptr) {
        avahi_client_free(this->client_);
        this->client_ = nullptr;
        this->group_ = nullptr;
    }
    this->reconnect_.store(false);
    int error = 0;
    // NO_FAIL: a daemon that is not running yet is waited for
    this->client_ = avahi_client_new(avahi_threaded_poll_get(this->poll_), AVAHI_CLIENT_NO_FAIL,
                                     &MdnsAdvertiser::on_client_state, this, &error);
    if (this->client_ == nullptr) {
        // Tried again on every refresh; said once
        if (!std::exchange(this->warned_, true)) {
            qCWarning(lc_mdns, "avahi is unavailable (%s); servers will not find this PC",
                      avahi_strerror(error));
        }
        this->reconnect_.store(true);
    } else {
        this->warned_ = false;
    }
}

void MdnsAdvertiser::refresh_() {
    const QList<Announcement> current = announcements(system_interfaces());

    avahi_threaded_poll_lock(this->poll_);
    if (this->reconnect_.load()) {
        this->announcements_ = current;
        this->connect_();
    } else if (current != this->announcements_ || this->republish_) {
        this->republish_ = false;
        this->announcements_ = current;
        if (this->client_ != nullptr &&
            avahi_client_get_state(this->client_) == AVAHI_CLIENT_S_RUNNING) {
            this->publish_(this->client_);
        }
    }
    avahi_threaded_poll_unlock(this->poll_);
}

/// Poll thread, or under its lock. client is passed in because the first state arrives before
/// avahi_client_new() has returned it.
void MdnsAdvertiser::publish_(AvahiClient* client) {
    if (this->group_ == nullptr) {
        this->group_ = avahi_entry_group_new(client, &MdnsAdvertiser::on_group_state, this);
        if (this->group_ == nullptr) {
            qCWarning(lc_mdns, "no avahi entry group: %s",
                      avahi_strerror(avahi_client_errno(client)));
            return;
        }
    }
    avahi_entry_group_reset(this->group_);
    if (this->announcements_.isEmpty()) {
        return;
    }

    AvahiStringList* txt = avahi_string_list_add_pair(nullptr, "path", "/sendspin");
    txt = avahi_string_list_add_pair(txt, "name", this->txt_name_.constData());
    int result = 0;
    for (const Announcement& announcement : this->announcements_) {
        for (const QHostAddress& ip : announcement.addresses) {
            AvahiAddress address;
            if (!to_avahi(ip, address)) {
                continue;
            }
            // NO_REVERSE: the address's PTR record belongs to the machine's own host name
            result = avahi_entry_group_add_address(this->group_, announcement.if_index,
                                                   AVAHI_PROTO_UNSPEC, AVAHI_PUBLISH_NO_REVERSE,
                                                   this->host_name_.constData(), &address);
            if (result < 0) {
                break;
            }
        }
        if (result < 0) {
            break;
        }
        result = avahi_entry_group_add_service_strlst(
            this->group_, announcement.if_index, AVAHI_PROTO_UNSPEC, AvahiPublishFlags{},
            this->name_.constData(), "_sendspin._tcp", nullptr, this->host_name_.constData(),
            this->port_, txt);
        if (result < 0) {
            break;
        }
    }
    avahi_string_list_free(txt);

    if (result >= 0) {
        result = avahi_entry_group_commit(this->group_);
    }
    if (result < 0) {
        qCWarning(lc_mdns, "advertising failed: %s", avahi_strerror(result));
        avahi_entry_group_reset(this->group_);
    }
}

void MdnsAdvertiser::on_client_state(AvahiClient* client, AvahiClientState state, void* data) {
    auto* self = static_cast<MdnsAdvertiser*>(data);
    switch (state) {
        case AVAHI_CLIENT_S_RUNNING:
            self->publish_(client);
            break;
        case AVAHI_CLIENT_S_COLLISION:
        case AVAHI_CLIENT_S_REGISTERING:
            // The daemon is settling its own host name; S_RUNNING follows
            if (self->group_ != nullptr) {
                avahi_entry_group_reset(self->group_);
            }
            break;
        case AVAHI_CLIENT_FAILURE:
            qCWarning(lc_mdns, "avahi: %s", avahi_strerror(avahi_client_errno(client)));
            // The client cannot be freed from its own callback
            self->reconnect_.store(true);
            break;
        case AVAHI_CLIENT_CONNECTING:
            break;
    }
}

void MdnsAdvertiser::on_group_state(AvahiEntryGroup* group, AvahiEntryGroupState state,
                                    void* data) {
    auto* self = static_cast<MdnsAdvertiser*>(data);
    switch (state) {
        case AVAHI_ENTRY_GROUP_ESTABLISHED:
            qCInfo(lc_mdns, "advertising \"%s\" on port %u", self->name_.constData(), self->port_);
            break;
        case AVAHI_ENTRY_GROUP_COLLISION: {
            // Either name may be the taken one, and avahi does not say which
            char* name = avahi_alternative_service_name(self->name_.constData());
            char* host = avahi_alternative_host_name(
                self->host_name_.first(self->host_name_.size() - 6).constData());
            self->name_ = name;
            self->host_name_ = QByteArray(host) + ".local";
            avahi_free(name);
            avahi_free(host);
            self->publish_(avahi_entry_group_get_client(group));
            break;
        }
        case AVAHI_ENTRY_GROUP_FAILURE:
            qCWarning(lc_mdns, "advertising failed: %s",
                      avahi_strerror(avahi_client_errno(avahi_entry_group_get_client(group))));
            self->republish_ = true;
            break;
        case AVAHI_ENTRY_GROUP_UNCOMMITED:
        case AVAHI_ENTRY_GROUP_REGISTERING:
            break;
    }
}
