#include "persistence.h"

#include <QTemporaryDir>
#include <QTest>

#include <sendspin/persistence_keys.h>
#include <sendspin/server/persistence_keys.h>

#include <sys/stat.h>

#include <array>

namespace {

mode_t mode_of(const std::filesystem::path& path) {
    struct stat st{};
    return ::stat(path.c_str(), &st) == 0 ? (st.st_mode & 07777) : 0;
}

}  // namespace

class PersistenceTest : public QObject {
    Q_OBJECT

private slots:
    void init() {
        this->tmp_ = std::make_unique<QTemporaryDir>();
        QVERIFY(this->tmp_->isValid());
        this->dir_ = std::filesystem::path(this->tmp_->path().toStdString()) / "sendspin";
    }

    void dataDirFollowsXdgDataHome() {
        qputenv("XDG_DATA_HOME", this->tmp_->path().toUtf8());
        QCOMPARE(data_dir(), this->dir_);
    }

    void serverBlobsRoundTripAsOwnerOnlyFiles() {
        namespace keys = sendspin::server::persistence_keys;
        std::vector<uint8_t> key(keys::SERVER_KEY_SIZE, 0xA5);
        std::vector<uint8_t> record(keys::CLIENT_RECORD_SIZE, 0x5A);

        ServerPersistence server(this->dir_);
        QVERIFY(!server.load_blob(keys::SERVER_KEY));
        QVERIFY(server.save_blob(keys::SERVER_KEY, key.data(), key.size()));
        QVERIFY(server.save_blob(keys::client_record_key(1023), record.data(), record.size()));
        QVERIFY(server.commit());

        ServerPersistence reopened(this->dir_);
        QCOMPARE(reopened.load_blob(keys::SERVER_KEY), std::optional(key));
        QCOMPARE(reopened.load_blob(keys::client_record_key(1023)), std::optional(record));
        QCOMPARE(mode_of(this->dir_ / "server" / keys::SERVER_KEY), mode_t{0600});
        QCOMPARE(mode_of(this->dir_ / "server" / "client_1023"), mode_t{0600});
    }

    void clientBlobsRoundTripAsOwnerOnlyFiles() {
        namespace keys = sendspin::persistence_keys;
        std::array<uint8_t, keys::KEYPAIR_SIZE> keypair;
        keypair.fill(0xC3);
        const std::string_view slot_key = "rec_254";
        QCOMPARE(keys::RecordSlotKey(254).view(), slot_key);
        std::array<uint8_t, keys::RECORD_SLOT_SIZE> slot;
        slot.fill(0x3C);

        ClientPersistence client(this->dir_);
        std::array<uint8_t, keys::RECORD_SLOT_SIZE> out{};
        QVERIFY(!client.read_blob(keys::KEYPAIR, out));
        QVERIFY(client.write_blob(keys::KEYPAIR, keypair));
        QVERIFY(client.write_blob(slot_key, slot));
        QVERIFY(client.commit());

        ClientPersistence reopened(this->dir_);
        QCOMPARE(reopened.read_blob(keys::KEYPAIR, out), std::optional(keypair.size()));
        QVERIFY(std::equal(keypair.begin(), keypair.end(), out.begin()));
        QCOMPARE(reopened.read_blob(slot_key, out), std::optional(slot.size()));
        QCOMPARE(out, slot);
        QCOMPARE(mode_of(this->dir_ / "client" / keys::KEYPAIR), mode_t{0600});
        QCOMPARE(mode_of(this->dir_ / "client" / slot_key), mode_t{0600});
    }

    void clientReadRejectsABlobLongerThanTheBuffer() {
        ClientPersistence client(this->dir_);
        QVERIFY(client.write_blob("k", std::array<uint8_t, 4>{1, 2, 3, 4}));
        std::array<uint8_t, 3> small{};
        QVERIFY(!client.read_blob("k", small));
        QCOMPARE(small, (std::array<uint8_t, 3>{}));
    }

    void serverAndClientDoNotShareKeys() {
        ServerPersistence server(this->dir_);
        ClientPersistence client(this->dir_);
        const uint8_t byte = 1;
        QVERIFY(server.save_blob("k", &byte, 1));
        std::array<uint8_t, 1> out{};
        QVERIFY(!client.read_blob("k", out));
    }

private:
    std::unique_ptr<QTemporaryDir> tmp_;
    std::filesystem::path dir_;
};

QTEST_APPLESS_MAIN(PersistenceTest)
#include "persistence_test.moc"
