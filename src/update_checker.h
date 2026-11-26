#pragma once

#include <QMap>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QObject>
#include <QPointer>
#include <QTimer>
#include <QUrl>
#include <QVersionNumber>

#include <optional>

/// @brief A published release, as far as updating needs it
struct Release {
    QVersionNumber version;
    /// Its page on GitHub
    QUrl page;
    /// Each file's download address, by its name
    QMap<QString, QUrl> files;
};

namespace update {

/// @brief Where a release and its files may come from: GitHub over https, or the stand-in the
/// check itself goes to (`$SENDSPIN_UPDATE_URL`, a test's server)
/// @param releases where the check goes
bool trusted(const QUrl& url, const QUrl& releases);

/// No reply about a release, and no SHA256SUMS, is ever near this
constexpr qint64 MAX_TEXT_BYTES = 1 << 20;

}  // namespace update

/// @brief Asks GitHub for the latest published release once a day, hourly until one check is
/// answered, and keeps the answer between runs. The app's only request that nobody clicked for;
/// the setting `updates/check` turns it off.
class UpdateChecker : public QObject {
    Q_OBJECT

public:
    /// @brief GitHub's "latest release" of this project, or `$SENDSPIN_UPDATE_URL` for a
    /// stand-in. Whatever answers there, nothing installs without the signature.
    static QUrl releases_url();
    /// @brief The reply's release, or the one kept from it; nullopt for anything else
    static std::optional<Release> parse(const QByteArray& json);
    static bool stored_enabled();

    /// @param current this build's version; null (a test, a build without one) never checks
    UpdateChecker(const QVersionNumber& current, const QUrl& url, QObject* parent = nullptr);

    /// @brief Checks when a day has passed since the last answer, now or once it has
    void start();
    /// @brief Asks now, whatever the setting and the day say
    void check();

    bool enabled() const;
    /// @brief Off forgets the release it knew of
    void set_enabled(bool enabled);
    /// @brief The latest release when it is newer than this build
    std::optional<Release> update() const;

signals:
    /// @brief update() or enabled() changed
    void changed();
    /// @brief A check ended, with an answer or without
    void checked(bool answered);

private:
    void on_reply_();
    /// parse(), less what is not trusted()
    std::optional<Release> read_(const QByteArray& json) const;

    const QVersionNumber current_;
    const QUrl url_;
    QNetworkAccessManager network_;
    QPointer<QNetworkReply> reply_;
    QTimer timer_;
    std::optional<Release> latest_;
};
