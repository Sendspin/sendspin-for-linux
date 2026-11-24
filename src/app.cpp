#include "app.h"

#include <QApplication>
#include <QIcon>
#include <QLoggingCategory>
#include <QStyle>
#include <QSysInfo>

#include <pipewire/pipewire.h>

Q_LOGGING_CATEGORY(lc_app, "sendspin.app")

namespace {

sendspin::SendspinClientConfig client_config() {
    sendspin::SendspinClientConfig config;
    config.name = QSysInfo::machineHostName().toStdString();
    return config;
}

}  // namespace

App::App(const std::filesystem::path& data_dir, QObject* parent)
    : QObject(parent),
      server_persistence_(data_dir),
      client_persistence_(data_dir),
      client_(client_config()) {
    this->client_.set_persistence_provider(&this->client_persistence_);
    qCInfo(lc_app, "PipeWire %s", pw_get_library_version());

    this->menu_.addAction(tr("Not connected"))->setEnabled(false);
    this->menu_.addSeparator();
    this->menu_.addAction(tr("Settings…"))->setEnabled(false);
    this->menu_.addAction(tr("Quit"), qApp, &QCoreApplication::quit);

    this->tray_.setIcon(QIcon::fromTheme(QIcon::ThemeIcon::MultimediaPlayer,
                                         qApp->style()->standardIcon(QStyle::SP_MediaVolume)));
    this->tray_.setToolTip(tr("Sendspin"));
    this->tray_.setContextMenu(&this->menu_);
    this->tray_.show();
}

void App::onSecondLaunch() {
    // Opens the Settings window once there is one
    qCInfo(lc_app, "second launch");
    this->tray_.showMessage(tr("Sendspin"), tr("Sendspin is already running."));
}
