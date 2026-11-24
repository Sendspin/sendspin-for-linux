#include "mdns_advertiser.h"

#include <QTest>

namespace {

NetInterface lan(const QString& name, int index, QList<NetInterface::Address> addresses) {
    NetInterface interface;
    interface.name = name;
    interface.index = index;
    interface.running = true;
    interface.addresses = std::move(addresses);
    return interface;
}

const NetInterface::Address V4{QHostAddress("192.168.1.20"), false};

}  // namespace

class MdnsAdvertiserTest : public QObject {
    Q_OBJECT

private slots:
    void announcesLanInterfaces() {
        const auto result = announcements(
            {lan("enp3s0", 2, {V4}), lan("wlp2s0", 3, {{QHostAddress("10.0.1.47"), false}})});
        QCOMPARE(result.size(), 2);
        QCOMPARE(result[0], (Announcement{2, {QHostAddress("192.168.1.20")}}));
        QCOMPARE(result[1], (Announcement{3, {QHostAddress("10.0.1.47")}}));
    }

    void skipsVirtualAndVpnInterfaces_data() {
        QTest::addColumn<QString>("name");
        for (const char* name :
             {"docker0", "br-3f2a1c9d8e7b", "veth1a2b3c", "virbr0", "vnet4", "vmnet8", "vboxnet0",
              "lxcbr0", "lxdbr0", "podman1", "cni0", "flannel.1", "cali1234", "tun0", "tap0", "wg0",
              "ztabcdef12", "tailscale0", "nordlynx"}) {
            QTest::newRow(name) << QString::fromLatin1(name);
        }
    }

    void skipsVirtualAndVpnInterfaces() {
        QFETCH(QString, name);
        QVERIFY(announcements({lan(name, 7, {V4})}).isEmpty());
    }

    void keepsABridgeOfTheLan() {
        QCOMPARE(announcements({lan("br0", 4, {V4})}).size(), 1);
    }

    void skipsLoopbackDownAndPointToPoint() {
        NetInterface loopback = lan("lo", 1, {{QHostAddress("127.0.0.1"), false}});
        loopback.loopback = true;
        NetInterface down = lan("enp4s0", 5, {V4});
        down.running = false;
        NetInterface tunnel = lan("ppp0", 6, {V4});
        tunnel.point_to_point = true;
        QVERIFY(announcements({loopback, down, tunnel}).isEmpty());
    }

    void skipsTemporaryAndLinkLocalAddresses() {
        const auto result =
            announcements({lan("enp3s0", 2,
                               {V4,
                                {QHostAddress("2001:db8::1"), false},
                                {QHostAddress("2001:db8::5dd2:4d7e:511e:1557"), true},
                                {QHostAddress("fe80::1%enp3s0"), true},
                                {QHostAddress("169.254.10.1"), false}})});
        QCOMPARE(result.size(), 1);
        QCOMPARE(result[0].addresses,
                 (QList<QHostAddress>{QHostAddress("192.168.1.20"), QHostAddress("2001:db8::1")}));
    }

    void dropsAnInterfaceWithNoAddressLeft() {
        QVERIFY(announcements({lan("enp3s0", 2, {{QHostAddress("fe80::1"), true}})}).isEmpty());
    }
};

QTEST_GUILESS_MAIN(MdnsAdvertiserTest)
#include "mdns_advertiser_test.moc"
