#include "update_installer.h"

#include <QFile>
#include <QFileInfo>
#include <QLoggingCategory>
#include <QNetworkReply>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QSysInfo>

#include <chrono>
#include <utility>

Q_LOGGING_CATEGORY(lc_update_install, "sendspin.update.install")

using namespace std::chrono_literals;
using update::Origin;

namespace {

constexpr const char* PACKAGE = "sendspin-for-linux";
constexpr const char* SUMS = "SHA256SUMS";
constexpr const char* SIGNATURE = "SHA256SUMS.asc";
/// pkexec's own: the prompt was dismissed, or nobody may authorize
constexpr int NOT_AUTHORIZED = 126;
/// install-update takes no larger a package
constexpr qint64 MAX_PACKAGE_BYTES = 256 << 20;
/// Without a byte for this long, a download has failed
constexpr std::chrono::seconds STALLED = 60s;

bool is_deb(const QString& os_id) {
    return os_id == u"debian" || os_id == u"ubuntu";
}

}  // namespace

namespace update {

Origin apt_origin(const QString& policy) {
    // " *** 1.2.0 500" marks the installed version; the lines under it, indented further, are
    // where apt knows it from. dpkg's own status file is the one that is no repository.
    const QStringList lines = policy.split(u'\n');
    const qsizetype installed = lines.indexOf(QRegularExpression(QStringLiteral("^ \\*\\*\\* .*")));
    if (installed < 0) {
        return Origin::NONE;
    }
    for (qsizetype i = installed + 1; i < lines.size() && lines[i].startsWith(u"        "); ++i) {
        if (!lines[i].trimmed().endsWith(u"/var/lib/dpkg/status")) {
            return Origin::REPOSITORY;
        }
    }
    return Origin::LOCAL;
}

Origin dnf_origin(const QString& from_repo) {
    const QString repo = from_repo.trimmed();
    if (repo.isEmpty()) {
        return Origin::NONE;
    }
    // "@commandline" for a file, "<unknown>" for one rpm itself installed
    return repo.startsWith(u'@') || repo.startsWith(u'<') ? Origin::LOCAL : Origin::REPOSITORY;
}

QString package_name(const QVersionNumber& version, const QString& os_id, const QString& os_version,
                     const QString& cpu) {
    QString arch = cpu;
    QString extension;
    if (is_deb(os_id)) {
        extension = QStringLiteral("deb");
        arch = cpu == u"x86_64" ? QStringLiteral("amd64") : cpu;
    } else if (os_id == u"fedora") {
        extension = QStringLiteral("rpm");
        arch = cpu == u"arm64" ? QStringLiteral("aarch64") : cpu;
    } else {
        return {};
    }
    return QStringLiteral("%1-%2-%3%4-%5.%6")
        .arg(QLatin1String(PACKAGE), version.toString(), os_id, os_version, arch, extension);
}

}  // namespace update

UpdateInstaller::System UpdateInstaller::this_system() {
    System system{.helper = QStringLiteral(SENDSPIN_UPDATE_HELPER),
                  .pkexec = QStandardPaths::findExecutable(QStringLiteral("pkexec")),
                  .key = {},
                  .os_id = QSysInfo::productType(),
                  .os_version = QSysInfo::productVersion(),
                  .cpu = QSysInfo::currentCpuArchitecture(),
                  .sandboxed = QFileInfo::exists(QStringLiteral("/.flatpak-info")),
                  .image_based = QFileInfo::exists(QStringLiteral("/run/ostree-booted")),
                  .origin_command = {}};
    // The one the package installed, and the only one install-update reads as root
    const QString key = QStringLiteral("/usr/share/sendspin-for-linux/sendspin.asc");
    if (QFileInfo(key).size() > 0) {
        system.key = key;
    }
    if (is_deb(system.os_id)) {
        system.origin_command = {QStringLiteral("apt-cache"), QStringLiteral("policy"),
                                 QLatin1String(PACKAGE)};
    } else if (system.os_id == u"fedora") {
        // From what dnf has on disk: no reason to refresh a repository for this
        system.origin_command = {QStringLiteral("dnf"),       QStringLiteral("-C"),
                                 QStringLiteral("repoquery"), QStringLiteral("--installed"),
                                 QStringLiteral("--qf"),      QStringLiteral("%{from_repo}\n"),
                                 QLatin1String(PACKAGE)};
    }
    return system;
}

UpdateInstaller::UpdateInstaller(System system, QObject* parent)
    : QObject(parent), system_(std::move(system)) {
    connect(&this->helper_, &QProcess::finished, this, &UpdateInstaller::on_helper_finished_);
    connect(&this->helper_, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        // Anything later ends in finished()
        if (error == QProcess::FailedToStart) {
            this->on_helper_finished_(-1, QProcess::CrashExit);
        }
    });
    // What it says is in the journal with everything else
    this->helper_.setProcessChannelMode(QProcess::ForwardedChannels);
}

void UpdateInstaller::probe() {
    if (this->probing_ || this->probed_) {
        return;
    }
    const auto done = [this](Origin origin) {
        this->origin_ = origin;
        this->probing_ = false;
        this->probed_ = true;
        emit this->changed();
    };
    if (this->system_.sandboxed || this->system_.origin_command.isEmpty()) {
        // Queued, so it ends after this returns either way
        QMetaObject::invokeMethod(this, [done] { done(Origin::NONE); }, Qt::QueuedConnection);
        this->probing_ = true;
        return;
    }
    this->probing_ = true;
    auto* query = new QProcess(this);
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    // apt-cache's output is parsed
    environment.insert(QStringLiteral("LC_ALL"), QStringLiteral("C"));
    query->setProcessEnvironment(environment);
    const auto finish = [this, query, done] {
        const QString output = QString::fromUtf8(query->readAllStandardOutput());
        query->deleteLater();
        done(is_deb(this->system_.os_id) ? update::apt_origin(output) : update::dnf_origin(output));
    };
    connect(query, &QProcess::finished, this, finish);
    connect(query, &QProcess::errorOccurred, this, [finish](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) {
            finish();
        }
    });
    query->start(this->system_.origin_command.first(), this->system_.origin_command.mid(1),
                 QIODevice::ReadOnly);
}

QString UpdateInstaller::package_(const Release& release) const {
    return update::package_name(release.version, this->system_.os_id, this->system_.os_version,
                                this->system_.cpu);
}

bool UpdateInstaller::can_install(const Release& release) const {
    const QString package = this->package_(release);
    return this->probed_ && !this->system_.sandboxed && !this->system_.image_based &&
           this->origin_ == Origin::LOCAL && !this->system_.key.isEmpty() &&
           !this->system_.pkexec.isEmpty() && QFileInfo::exists(this->system_.helper) &&
           !package.isEmpty() && release.files.contains(package) &&
           release.files.contains(QLatin1String(SUMS)) &&
           release.files.contains(QLatin1String(SIGNATURE));
}

bool UpdateInstaller::busy() const {
    return this->state_ == State::DOWNLOADING || this->state_ == State::VERIFYING ||
           this->state_ == State::INSTALLING;
}

void UpdateInstaller::install(const Release& release) {
    if (this->busy() || this->state_ == State::INSTALLED || !this->can_install(release)) {
        return;
    }
    this->dir_ = std::make_unique<QTemporaryDir>();
    if (!this->dir_->isValid()) {
        this->set_state_(State::FAILED, Failure::DOWNLOAD);
        return;
    }
    const QString package = this->package_(release);
    this->files_ = {this->dir_->filePath(package), this->dir_->filePath(QLatin1String(SUMS)),
                    this->dir_->filePath(QLatin1String(SIGNATURE))};
    this->percent_ = 0;
    ++this->attempt_;
    this->fetching_ = 0;
    this->set_state_(State::DOWNLOADING);
    this->fetch_(release.files.value(package), package, true);
    this->fetch_(release.files.value(QLatin1String(SUMS)), QLatin1String(SUMS), false);
    this->fetch_(release.files.value(QLatin1String(SIGNATURE)), QLatin1String(SIGNATURE), false);
}

void UpdateInstaller::fetch_(const QUrl& url, const QString& name, bool is_package) {
    QNetworkRequest request(url);
    request.setTransferTimeout(STALLED);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::UserVerifiedRedirectPolicy);
    QNetworkReply* reply = this->network_.get(request);
    // GitHub hands a download on to its storage; nowhere else is followed
    connect(reply, &QNetworkReply::redirected, this, [reply, url](const QUrl& to) {
        if (update::trusted(to, url)) {
            emit reply->redirectAllowed();
        } else {
            reply->abort();
        }
    });
    const qint64 limit = is_package ? MAX_PACKAGE_BYTES : update::MAX_TEXT_BYTES;
    auto* file = new QFile(this->dir_->filePath(name), reply);
    const bool opened = file->open(QIODevice::WriteOnly);
    ++this->fetching_;
    const int attempt = this->attempt_;
    connect(reply, &QNetworkReply::readyRead, this, [reply, file, limit] {
        if (file->size() + reply->bytesAvailable() > limit) {
            reply->abort();
            return;
        }
        file->write(reply->readAll());
    });
    if (is_package) {
        connect(reply, &QNetworkReply::downloadProgress, this,
                [this, attempt](qint64 got, qint64 total) {
                    const int percent = total > 0 ? static_cast<int>(got * 100 / total) : 0;
                    if (attempt == this->attempt_ && percent != this->percent_ &&
                        this->state_ == State::DOWNLOADING) {
                        this->percent_ = percent;
                        emit this->changed();
                    }
                });
    }
    connect(reply, &QNetworkReply::finished, this, [this, reply, file, opened, name, attempt] {
        file->write(reply->readAll());
        // A write that failed shows in the file's own error, a full disk for one
        const bool ok = opened && reply->error() == QNetworkReply::NoError && file->flush() &&
                        file->error() == QFileDevice::NoError;
        file->close();
        reply->deleteLater();
        if (attempt != this->attempt_ || this->state_ != State::DOWNLOADING) {
            // Another of the three failed first, and this may be a later try already
            return;
        }
        --this->fetching_;
        if (!ok) {
            qCWarning(lc_update_install, "%s did not download: %s", qUtf8Printable(name),
                      qUtf8Printable(reply->errorString()));
            this->set_state_(State::FAILED, Failure::DOWNLOAD);
            return;
        }
        if (this->fetching_ == 0) {
            this->set_state_(State::VERIFYING);
            this->run_(this->system_.helper, QStringList{QStringLiteral("verify")} + this->files_);
        }
    });
}

void UpdateInstaller::run_(const QString& program, const QStringList& arguments) {
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    // Only `verify` reads it; tests have a key of their own
    environment.insert(QStringLiteral("SENDSPIN_UPDATE_KEY"), this->system_.key);
    this->helper_.setProcessEnvironment(environment);
    this->helper_.start(program, arguments);
}

void UpdateInstaller::on_helper_finished_(int code, QProcess::ExitStatus status) {
    const bool ok = status == QProcess::NormalExit && code == 0;
    if (this->state_ == State::VERIFYING) {
        if (status != QProcess::NormalExit) {
            // It never ran: nothing is known about the download
            qCWarning(lc_update_install, "the download could not be verified");
            this->set_state_(State::FAILED, Failure::INSTALL);
            return;
        }
        if (!ok) {
            qCWarning(lc_update_install, "the download did not verify, and is not installed");
            this->set_state_(State::FAILED, Failure::SIGNATURE);
            return;
        }
        // It verifies again as root, on a copy only root can change: what happens to these
        // files from here on cannot change what is installed
        this->set_state_(State::INSTALLING);
        this->run_(this->system_.pkexec,
                   QStringList{this->system_.helper, QStringLiteral("install")} + this->files_);
        return;
    }
    if (this->state_ != State::INSTALLING) {
        return;
    }
    this->dir_.reset();
    if (ok) {
        this->set_state_(State::INSTALLED);
        emit this->installed();
    } else if (status == QProcess::NormalExit && code == NOT_AUTHORIZED) {
        this->set_state_(State::IDLE);
    } else {
        qCWarning(lc_update_install, "installing the update failed (%d)", code);
        this->set_state_(State::FAILED, Failure::INSTALL);
    }
}

void UpdateInstaller::set_state_(State state, Failure failure) {
    this->state_ = state;
    this->failure_ = failure;
    emit this->changed();
}
