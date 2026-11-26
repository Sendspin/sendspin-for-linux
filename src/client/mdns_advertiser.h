#pragma once

#include <QByteArray>
#include <QHostAddress>
#include <QList>
#include <QObject>
#include <QString>
#include <QTimer>

#include <avahi-client/client.h>
#include <avahi-client/publish.h>
#include <avahi-common/thread-watch.h>

#include <atomic>
#include <cstdint>

/// @brief A network interface as the advertiser sees it
struct NetInterface {
    struct Address {
        QHostAddress ip;
        /// A temporary (RFC 8981) or deprecated address, which changes under a server that
        /// remembers it
        bool short_lived{false};

        bool operator==(const Address&) const = default;
    };

    QString name;
    int index{0};
    bool running{false};
    bool loopback{false};
    bool point_to_point{false};  ///< how VPN tunnels show up
    QList<Address> addresses;
};

/// @brief What is announced on one interface
struct Announcement {
    int if_index{0};
    QList<QHostAddress> addresses;

    bool operator==(const Announcement&) const = default;
};

QList<NetInterface> system_interfaces();

/// @brief Where a server on the local network can reach this PC: the running LAN interfaces,
/// without container, VM and VPN ones, each with its IPv4 addresses. An interface left with
/// no address is dropped.
QList<Announcement> announcements(const QList<NetInterface>& interfaces);

/// @brief Advertises the client as `_sendspin._tcp` through avahi-daemon, so servers find it and
/// connect. sendspin-cpp has no mDNS of its own.
///
/// The service points at a host name of its own with only the addresses announcements() picks,
/// instead of the machine's, whose records carry every address avahi-daemon knows.
class MdnsAdvertiser : public QObject {
    Q_OBJECT

public:
    explicit MdnsAdvertiser(QObject* parent = nullptr);
    ~MdnsAdvertiser() override;

    /// @brief Starts advertising, and keeps at it across network changes and daemon restarts.
    /// Without a daemon it waits for one.
    /// @param name the instance name and TXT `name`
    /// @param host_label a DNS label unique to this client, for its host name
    void start(const QString& name, const QByteArray& host_label, uint16_t port);
    void stop();

private:
    static void on_client_state(AvahiClient* client, AvahiClientState state, void* data);
    static void on_group_state(AvahiEntryGroup* group, AvahiEntryGroupState state, void* data);

    void refresh_();
    void connect_();
    void publish_(AvahiClient* client);

    QTimer refresh_timer_;
    AvahiThreadedPoll* poll_{nullptr};
    /// The daemon went away, or was never there: connect again from the main thread
    std::atomic<bool> reconnect_{false};

    // Below: under the poll's lock once it runs
    AvahiClient* client_{nullptr};
    AvahiEntryGroup* group_{nullptr};
    QByteArray name_;       ///< instance name, renamed on a collision
    QByteArray txt_name_;   ///< the name as given
    QByteArray host_name_;  ///< `<label>.local`, renamed on a collision
    uint16_t port_{0};
    QList<Announcement> announcements_;
    bool republish_{false};  ///< the group failed; publish again on the next refresh
    bool warned_{false};     ///< said once that avahi is unavailable
};
