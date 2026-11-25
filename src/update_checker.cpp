#include "update_checker.h"

#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLoggingCategory>
#include <QNetworkRequest>
#include <QSettings>

#include <algorithm>
#include <chrono>

Q_LOGGING_CATEGORY(lc_update_check, "sendspin.update")

using namespace std::chrono_literals;

namespace {

constexpr const char* KEY_CHECK = "updates/check";      ///< on unless set false
constexpr const char* KEY_LAST = "updates/last_check";  ///< seconds since the epoch
constexpr const char* KEY_LATEST = "updates/latest";    ///< what parse() takes
constexpr std::chrono::seconds BETWEEN_CHECKS = 24h;
constexpr std::chrono::seconds AFTER_A_FAILURE = 1h;
constexpr std::chrono::seconds REPLY_TIMEOUT = 30s;

QSettings store() {
    return QSettings(QStringLiteral("sendspin"), QStringLiteral("sendspin"));
}

/// The reply, cut down to what parse() reads
QByteArray kept(const Release& release) {
    QJsonArray assets;
    for (auto file = release.files.begin(); file != release.files.end(); ++file) {
        assets.append(
            QJsonObject{{"name", file.key()}, {"browser_download_url", file.value().toString()}});
    }
    return QJsonDocument(QJsonObject{{"tag_name", u'v' + release.version.toString()},
                                     {"html_url", release.page.toString()},
                                     {"assets", assets}})
        .toJson(QJsonDocument::Compact);
}

}  // namespace

QUrl UpdateChecker::releases_url() {
    const QString stand_in = qEnvironmentVariable("SENDSPIN_UPDATE_URL");
    return QUrl(stand_in.isEmpty()
                    ? QStringLiteral("https://api.github.com/repos/Sendspin/sendspin-for-linux/"
                                     "releases/latest")
                    : stand_in);
}

std::optional<Release> UpdateChecker::parse(const QByteArray& json) {
    const QJsonObject reply = QJsonDocument::fromJson(json).object();
    QString tag = reply.value("tag_name").toString();
    if (tag.startsWith(u'v')) {
        tag.remove(0, 1);
    }
    qsizetype suffix = 0;
    Release release{.version = QVersionNumber::fromString(tag, &suffix),
                    .page = QUrl(reply.value("html_url").toString()),
                    .files = {}};
    // "1.2.0-rc1" is not a release to offer
    if (release.version.isNull() || suffix != tag.size()) {
        return std::nullopt;
    }
    for (const QJsonValue& asset : reply.value("assets").toArray()) {
        const QUrl url(asset["browser_download_url"].toString());
        if (url.isValid() && !asset["name"].toString().isEmpty()) {
            release.files.insert(asset["name"].toString(), url);
        }
    }
    return release;
}

bool UpdateChecker::stored_enabled() {
    return store().value(KEY_CHECK, true).toBool();
}

UpdateChecker::UpdateChecker(const QVersionNumber& current, const QUrl& url, QObject* parent)
    : QObject(parent), current_(current), url_(url) {
    this->timer_.setSingleShot(true);
    connect(&this->timer_, &QTimer::timeout, this, &UpdateChecker::check);
}

void UpdateChecker::start() {
    if (this->current_.isNull() || !this->enabled()) {
        return;
    }
    const QSettings settings = store();
    this->latest_ = parse(settings.value(KEY_LATEST).toByteArray());
    const qint64 since =
        QDateTime::currentSecsSinceEpoch() - settings.value(KEY_LAST, 0).toLongLong();
    // A clock set back would otherwise put the next check off for as long
    const qint64 wait = since < 0 ? 0 : std::max<qint64>(0, BETWEEN_CHECKS.count() - since);
    this->timer_.start(std::chrono::seconds(wait));
    if (this->latest_) {
        emit this->changed();
    }
}

void UpdateChecker::check() {
    if (this->reply_) {
        return;
    }
    QNetworkRequest request(this->url_);
    request.setRawHeader("Accept", "application/vnd.github+json");
    // GitHub's API refuses a request without one
    request.setHeader(QNetworkRequest::UserAgentHeader,
                      QStringLiteral("sendspin-for-linux/%1").arg(this->current_.toString()));
    request.setTransferTimeout(REPLY_TIMEOUT);
    this->reply_ = this->network_.get(request);
    connect(this->reply_, &QNetworkReply::finished, this, &UpdateChecker::on_reply_);
}

void UpdateChecker::on_reply_() {
    QNetworkReply* reply = this->reply_;
    this->reply_.clear();
    reply->deleteLater();
    const std::optional<Release> release =
        reply->error() == QNetworkReply::NoError ? parse(reply->readAll()) : std::nullopt;
    if (!release) {
        qCInfo(lc_update_check, "no answer about the latest release: %s",
               qUtf8Printable(reply->errorString()));
        this->timer_.start(AFTER_A_FAILURE);
        emit this->checked(false);
        return;
    }
    QSettings settings = store();
    settings.setValue(KEY_LAST, QDateTime::currentSecsSinceEpoch());
    settings.setValue(KEY_LATEST, kept(*release));
    this->latest_ = release;
    if (this->enabled()) {
        this->timer_.start(BETWEEN_CHECKS);
    }
    emit this->changed();
    emit this->checked(true);
}

bool UpdateChecker::enabled() const {
    return stored_enabled();
}

void UpdateChecker::set_enabled(bool enabled) {
    if (enabled == this->enabled()) {
        return;
    }
    QSettings settings = store();
    settings.setValue(KEY_CHECK, enabled);
    if (enabled) {
        this->start();
    } else {
        this->timer_.stop();
        if (this->reply_) {
            this->reply_->disconnect(this);
            this->reply_->abort();
            this->reply_->deleteLater();
            this->reply_.clear();
        }
        settings.remove(KEY_LATEST);
        this->latest_.reset();
    }
    emit this->changed();
}

std::optional<Release> UpdateChecker::update() const {
    if (!this->latest_ || this->current_.isNull() || this->latest_->version <= this->current_) {
        return std::nullopt;
    }
    return this->latest_;
}
