// Configuration compatibility harness: loads and saves the shared settings
// file with the *real* desktop panel classes (AppConfig, ServerConfig, ...).
//
//   desktop_config dump <settings.conf>        -> JSON + generated wavers .conf
//   desktop_config set-log-level <file> <n>    -> like SettingsDialog::accept
//   desktop_config set-heartbeat <file> <ms>   -> like ServerConfigDialog::accept
//   desktop_config add-screen <file> <name>    -> adds a screen and saves
#include "AppConfig.h"
#include "ServerConfig.h"
#include "Hotkey.h"

#include <QApplication>
#include <QBuffer>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTextStream>
#include <cstdio>

class SettingsDialog {
public:
    static void setLogLevel(AppConfig& c, int level) { c.setLogLevel(level); c.saveSettings(); }
};

class ServerConfigDialog {
public:
    static void setHeartbeat(ServerConfig& c, int ms) { c.haveHeartbeat(true); c.setHeartbeat(ms); c.saveSettings(); }
    static void addScreen(ServerConfig& c, const QString& name) {
        for (auto& s : c.screens()) {
            if (s.isNull()) { s.setName(name); s.setPosition(QPoint(600, 20)); break; }
        }
        c.saveSettings();
    }
};

int main(int argc, char** argv)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    if (argc < 3) {
        fprintf(stderr, "usage: %s dump|set-log-level|set-heartbeat|add-screen <file> [value]\n", argv[0]);
        return 2;
    }
    const QString cmd = argv[1];
    // heap allocated and intentionally leaked: the destructors save settings
    QSettings* settings = new QSettings(QString::fromLocal8Bit(argv[2]), QSettings::IniFormat);
    AppConfig* appConfig = new AppConfig(settings);
    ServerConfig* config = new ServerConfig(settings, 5, 3, appConfig->screenName(), nullptr);

    if (cmd == "set-log-level" && argc > 3) {
        SettingsDialog::setLogLevel(*appConfig, QString(argv[3]).toInt());
        return 0;
    }
    if (cmd == "set-heartbeat" && argc > 3) {
        ServerConfigDialog::setHeartbeat(*config, QString(argv[3]).toInt());
        return 0;
    }
    if (cmd == "add-screen" && argc > 3) {
        ServerConfigDialog::addScreen(*config, argv[3]);
        return 0;
    }
    if (cmd != "dump") {
        return 2;
    }

    QJsonObject general;
    general["screenName"] = appConfig->screenName();
    general["port"] = appConfig->port();
    general["interface"] = appConfig->networkInterface();
    general["logLevel"] = appConfig->logLevelText();
    general["logToFile"] = appConfig->logToFile();
    general["cryptoEnabled"] = appConfig->getCryptoEnabled();
    general["uhidEnabled"] = appConfig->getUhidEnabled();
    general["uhidName"] = appConfig->getUhidName();
    general["language"] = appConfig->language();

    QJsonArray screens;
    for (const Screen& s : static_cast<const ServerConfig*>(config)->screens()) {
        if (s.isNull()) continue;
        QJsonObject o;
        o["name"] = s.name();
        o["x"] = s.position().x();
        o["y"] = s.position().y();
        QJsonArray aliases;
        for (const QString& a : s.aliases()) aliases.append(a);
        o["aliases"] = aliases;
        o["linkRight"] = s.link(Screen::LinkRight);
        o["linkLeft"] = s.link(Screen::LinkLeft);
        screens.append(o);
    }
    QJsonArray hotkeys;
    for (const Hotkey& h : static_cast<const ServerConfig*>(config)->hotkeys()) {
        QString text;
        QTextStream ts(&text);
        ts << h;
        ts.flush();
        hotkeys.append(text.trimmed());
    }

    QString confText;
    {
        QTextStream ts(&confText);
        ts << *config;
        ts.flush();
    }
    QJsonObject out;
    out["general"] = general;
    out["screens"] = screens;
    out["hotkeys"] = hotkeys;
    out["heartbeat"] = config->hasHeartbeat() ? config->heartbeat() : -1;
    out["useCustomLinks"] = config->useCustomLinks();
    out["serverConfigText"] = confText;
    fputs(QJsonDocument(out).toJson().constData(), stdout);
    return 0;
}
