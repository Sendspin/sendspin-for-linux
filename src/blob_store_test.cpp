#include "blob_store.h"

#include <QTemporaryDir>
#include <QTest>

#include <sys/stat.h>
#include <unistd.h>

#include <fstream>

namespace {

mode_t mode_of(const std::filesystem::path& path) {
    struct stat st{};
    return ::stat(path.c_str(), &st) == 0 ? (st.st_mode & 07777) : 0;
}

}  // namespace

class BlobStoreTest : public QObject {
    Q_OBJECT

private slots:
    void init() {
        this->tmp_ = std::make_unique<QTemporaryDir>();
        QVERIFY(this->tmp_->isValid());
        this->dir_ = std::filesystem::path(this->tmp_->path().toStdString()) / "sendspin" / "store";
    }

    void missingKeyLoadsNothing() {
        const BlobStore store(this->dir_);
        QVERIFY(!store.load("absent"));
        // Loading must not create anything
        QVERIFY(!std::filesystem::exists(this->dir_));
    }

    void roundTrips() {
        BlobStore store(this->dir_);
        const std::vector<uint8_t> blob{0x00, 0x01, 0xFF, 0x00, 0x7F};
        QVERIFY(store.save("server_key", blob));
        QCOMPARE(BlobStore(this->dir_).load("server_key"), std::optional(blob));
    }

    void roundTripsEmptyAndLargeBlobs() {
        BlobStore store(this->dir_);
        QVERIFY(store.save("empty", {}));
        QCOMPARE(store.load("empty"), std::optional(std::vector<uint8_t>{}));

        std::vector<uint8_t> large(10000);
        for (size_t i = 0; i < large.size(); ++i) {
            large[i] = static_cast<uint8_t>(i * 31);
        }
        QVERIFY(store.save("large", large));
        QCOMPARE(store.load("large"), std::optional(large));
    }

    void overwrites() {
        BlobStore store(this->dir_);
        QVERIFY(store.save("k", std::vector<uint8_t>{1, 2, 3, 4}));
        QVERIFY(store.save("k", std::vector<uint8_t>{9}));
        QCOMPARE(store.load("k"), std::optional(std::vector<uint8_t>{9}));
    }

    void filesAreOwnerOnly() {
        // The mode must not depend on the caller's umask
        const mode_t old_umask = ::umask(0);
        BlobStore store(this->dir_);
        const bool saved = store.save("k", std::vector<uint8_t>{1});
        ::umask(old_umask);
        QVERIFY(saved);
        QCOMPARE(mode_of(this->dir_), mode_t{0700});
        QCOMPARE(mode_of(this->dir_ / "k"), mode_t{0600});
    }

    void overwriteTightensALooseFile() {
        BlobStore store(this->dir_);
        QVERIFY(store.save("k", std::vector<uint8_t>{1}));
        QCOMPARE(::chmod((this->dir_ / "k").c_str(), 0644), 0);
        QVERIFY(store.save("k", std::vector<uint8_t>{2}));
        QCOMPARE(mode_of(this->dir_ / "k"), mode_t{0600});
    }

    void leavesNoTemporary() {
        BlobStore store(this->dir_);
        QVERIFY(store.save("k", std::vector<uint8_t>{1}));
        size_t entries = 0;
        for ([[maybe_unused]] const auto& entry : std::filesystem::directory_iterator(this->dir_)) {
            ++entries;
        }
        QCOMPARE(entries, size_t{1});
    }

    void rejectsKeysThatAreNotFileNames_data() {
        QTest::addColumn<QByteArray>("key");
        QTest::newRow("empty") << QByteArray("");
        QTest::newRow("parent") << QByteArray("..");
        QTest::newRow("traversal") << QByteArray("../escape");
        QTest::newRow("absolute") << QByteArray("/tmp/escape");
        QTest::newRow("subdir") << QByteArray("a/b");
        QTest::newRow("hidden") << QByteArray(".k");
        QTest::newRow("nul") << QByteArray("a\0b", 3);
    }

    void rejectsKeysThatAreNotFileNames() {
        QFETCH(QByteArray, key);
        BlobStore store(this->dir_);
        const std::string_view view(key.constData(), static_cast<size_t>(key.size()));
        QVERIFY(!store.save(view, std::vector<uint8_t>{1}));
        QVERIFY(!store.load(view));
        QVERIFY(!std::filesystem::exists(this->dir_.parent_path() / "escape"));
    }

    void failedSaveKeepsTheOldBlob() {
        if (::geteuid() == 0) {
            QSKIP("root writes to a read-only directory");
        }
        BlobStore store(this->dir_);
        QVERIFY(store.save("k", std::vector<uint8_t>{1}));
        QCOMPARE(::chmod(this->dir_.c_str(), 0500), 0);
        const bool saved = store.save("k", std::vector<uint8_t>{2});
        QCOMPARE(::chmod(this->dir_.c_str(), 0700), 0);
        QVERIFY(!saved);
        QCOMPARE(store.load("k"), std::optional(std::vector<uint8_t>{1}));
    }

private:
    std::unique_ptr<QTemporaryDir> tmp_;
    std::filesystem::path dir_;
};

QTEST_APPLESS_MAIN(BlobStoreTest)
#include "blob_store_test.moc"
