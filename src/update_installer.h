#pragma once

#include "update_checker.h"

#include <QNetworkAccessManager>
#include <QObject>
#include <QProcess>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>

#include <memory>

namespace update {

/// Where the installed package came from, which decides who updates it
enum class Origin {
    NONE,        ///< no package: a build from source, another distro's packaging
    LOCAL,       ///< a downloaded file: nothing else will update it
    REPOSITORY,  ///< an apt or dnf repository, whose updates bring the next one
};

/// @param policy what `apt-cache policy sendspin-for-linux` prints
Origin apt_origin(const QString& policy);
/// @param from_repo what `dnf repoquery --installed --qf '%{from_repo}'` prints
Origin dnf_origin(const QString& from_repo);

/// @brief The name of a release's package for a system, as the release workflow names them
/// @param os_id,os_version ID and VERSION_ID of /etc/os-release
/// @param cpu as QSysInfo::currentCpuArchitecture() has it
/// @return empty where no package is built for the distro
QString package_name(const QVersionNumber& version, const QString& os_id, const QString& os_version,
                     const QString& cpu);

}  // namespace update

/// @brief Takes a release's package for this system to an installed one: downloads it with the
/// release's signed SHA256SUMS, has `install-update` verify them against the key the package
/// came with, and runs that again through pkexec to install. Only where nothing else
/// updates the app: a package installed from a downloaded file, not from a repository, not the
/// Flatpak.
class UpdateInstaller : public QObject {
    Q_OBJECT

public:
    /// What this machine is, and the tools on it
    struct System {
        QString helper;  ///< install-update, as the package installed it
        QString pkexec;  ///< empty for none
        QString key;     ///< the signing key the package installed; empty for none
        QString os_id;
        QString os_version;
        QString cpu;
        bool sandboxed{false};  ///< the Flatpak
        /// Prints what apt_origin() or dnf_origin() reads, by os_id; empty where neither is
        QStringList origin_command;
    };
    static System this_system();

    enum class State { IDLE, DOWNLOADING, VERIFYING, INSTALLING, INSTALLED, FAILED };
    enum class Failure { NONE, DOWNLOAD, SIGNATURE, INSTALL };

    explicit UpdateInstaller(System system, QObject* parent = nullptr);

    /// @brief Asks the package manager where the package came from, once. can_install() is
    /// false until it answered.
    void probe();
    bool probed() const {
        return this->probed_;
    }
    update::Origin origin() const {
        return this->origin_;
    }
    bool sandboxed() const {
        return this->system_.sandboxed;
    }
    /// @brief This machine can verify and install this release itself
    bool can_install(const Release& release) const;

    /// @brief Downloads, verifies and installs; installed() or a FAILED state ends it. A
    /// password prompt that is dismissed ends it IDLE.
    void install(const Release& release);

    State state() const {
        return this->state_;
    }
    bool busy() const;
    /// @brief Why the state is FAILED
    Failure failure() const {
        return this->failure_;
    }
    /// @brief How much of the package is here, 0-100, while DOWNLOADING
    int percent() const {
        return this->percent_;
    }

signals:
    /// @brief Anything above changed
    void changed();
    /// @brief The new version is on disk: time to restart
    void installed();

private:
    QString package_(const Release& release) const;
    void fetch_(const QUrl& url, const QString& name, bool is_package);
    void run_(const QString& program, const QStringList& arguments);
    void on_helper_finished_(int code, QProcess::ExitStatus status);
    void set_state_(State state, Failure failure = Failure::NONE);

    const System system_;
    bool probing_{false};
    bool probed_{false};
    update::Origin origin_{update::Origin::NONE};

    State state_{State::IDLE};
    Failure failure_{Failure::NONE};
    int percent_{0};
    QNetworkAccessManager network_;
    /// Downloads of this attempt still under way
    int fetching_{0};
    int attempt_{0};
    /// The download, its sums and their signature, as install-update takes them
    QStringList files_;
    std::unique_ptr<QTemporaryDir> dir_;
    QProcess helper_;
};
