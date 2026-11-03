/*
 * Etherwaver -- remote control panel of the client window.
 *
 * This package is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * found in the file LICENSE that should have accompanied this file.
 *
 * This package is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include "RemoteControlPanel.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QFile>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QNetworkProxy>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPushButton>
#include <QSettings>
#include <QUrl>
#include <QUrlQuery>
#include <QVBoxLayout>

namespace {

const char kDefaultHotkey[] = "Ctrl+Alt+Shift+Q";
const int  kDefaultPort = 24803;

} // namespace

RemoteControlPanel::RemoteControlPanel(QSettings& settings, const QString& profileDir,
                                       QWidget* parent) :
    QGroupBox(parent),
    m_settings(settings),
    m_profileDir(profileDir),
    m_port(settings.value("remoteControl/apiPort", kDefaultPort).toInt()),
    m_running(false),
    m_reachable(false)
{
    setTitle(tr("Control the server with this computer's mouse and keyboard"));

    m_pStateLabel = new QLabel(this);
    QFont bold = m_pStateLabel->font();
    bold.setBold(true);
    m_pStateLabel->setFont(bold);

    m_pInfoLabel = new QLabel(this);
    m_pInfoLabel->setWordWrap(true);
    m_pWarningLabel = new QLabel(this);
    m_pWarningLabel->setWordWrap(true);
    m_pWarningLabel->setStyleSheet("color: #b05000;");
    m_pWarningLabel->hide();

    m_pModeCombo = new QComboBox(this);
    m_pModeCombo->addItem(tr("Full capture (all keys, mouse locked)"), "capture");
    m_pModeCombo->addItem(tr("Session window (only while the window has focus)"), "window");
    m_pModeCombo->setCurrentIndex(
        m_pModeCombo->findData(settings.value("remoteControl/mode", "capture").toString()));

    m_pTargetEdit = new QLineEdit(this);
    m_pTargetEdit->setPlaceholderText(tr("the server"));
    m_pTargetEdit->setToolTip(tr("Name of the computer to control; leave empty to control the server itself."));
    m_pTargetEdit->setText(settings.value("remoteControl/target").toString());

    m_pHotkeyEdit = new QLineEdit(this);
    m_pHotkeyEdit->setText(releaseHotkey());
    m_pHotkeyEdit->setToolTip(tr("Pressing this shortcut on this computer always ends remote control, "
                                 "even when the connection is lost. It is never sent to the server. "
                                 "A change takes effect when the client is restarted."));

    m_pStartButton    = new QPushButton(tr("Control with local mouse and keyboard"), this);
    m_pTakeoverButton = new QPushButton(tr("Take over control"), this);
    m_pStopButton     = new QPushButton(tr("Release mouse and keyboard"), this);
    m_pCadButton      = new QPushButton(tr("Send Ctrl+Alt+Del"), this);
    m_pTakeoverButton->hide();

    QGridLayout* form = new QGridLayout();
    form->addWidget(new QLabel(tr("Mode:"), this), 0, 0);
    form->addWidget(m_pModeCombo, 0, 1);
    form->addWidget(new QLabel(tr("Computer:"), this), 1, 0);
    form->addWidget(m_pTargetEdit, 1, 1);
    form->addWidget(new QLabel(tr("Emergency release shortcut:"), this), 2, 0);
    form->addWidget(m_pHotkeyEdit, 2, 1);

    QHBoxLayout* buttons = new QHBoxLayout();
    buttons->addWidget(m_pStartButton);
    buttons->addWidget(m_pTakeoverButton);
    buttons->addWidget(m_pStopButton);
    buttons->addWidget(m_pCadButton);
    buttons->addStretch();

    QVBoxLayout* layout = new QVBoxLayout(this);
    layout->addWidget(m_pStateLabel);
    layout->addWidget(m_pInfoLabel);
    layout->addWidget(m_pWarningLabel);
    layout->addLayout(form);
    layout->addLayout(buttons);

    // the API is on loopback: never go through a proxy
    m_net.setProxy(QNetworkProxy::NoProxy);
    connect(&m_net, &QNetworkAccessManager::finished, this, &RemoteControlPanel::handleReply);
    connect(&m_timer, &QTimer::timeout, this, &RemoteControlPanel::poll);
    connect(m_pStartButton, &QPushButton::clicked, this, &RemoteControlPanel::startClicked);
    connect(m_pTakeoverButton, &QPushButton::clicked, this, &RemoteControlPanel::takeoverClicked);
    connect(m_pStopButton, &QPushButton::clicked, this, &RemoteControlPanel::stopClicked);
    connect(m_pCadButton, &QPushButton::clicked, this, &RemoteControlPanel::ctrlAltDelClicked);
    connect(m_pHotkeyEdit, &QLineEdit::editingFinished, this, &RemoteControlPanel::hotkeyEdited);
    connect(m_pModeCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), [this](int) {
        m_settings.setValue("remoteControl/mode", m_pModeCombo->currentData().toString());
    });
    connect(m_pTargetEdit, &QLineEdit::editingFinished, [this]() {
        m_settings.setValue("remoteControl/target", m_pTargetEdit->text().trimmed());
    });

    m_timer.setInterval(700);
    updateUi();
}

QString
RemoteControlPanel::releaseHotkey() const
{
    const QString value = m_settings.value("remoteControl/releaseHotkey", kDefaultHotkey).toString().trimmed();
    return value.isEmpty() ? QString(kDefaultHotkey) : value;
}

void
RemoteControlPanel::hotkeyEdited()
{
    QString value = m_pHotkeyEdit->text().trimmed();
    // a shortcut needs at least one modifier and a key
    if (!value.contains('+')) {
        m_pHotkeyEdit->setText(releaseHotkey());
        return;
    }
    m_settings.setValue("remoteControl/releaseHotkey", value);
}

void
RemoteControlPanel::setClientRunning(bool running)
{
    if (m_running == running) {
        return;
    }
    m_running = running;
    m_reachable = false;
    m_status = QJsonObject();
    m_token.clear();
    if (running) {
        m_timer.start();
        poll();
    }
    else {
        m_timer.stop();
    }
    updateUi();
}

bool
RemoteControlPanel::loadToken()
{
    QFile file(QDir(m_profileDir).filePath("remote-control-api.json"));
    if (!file.open(QIODevice::ReadOnly)) {
        return false;
    }
    const QJsonObject o = QJsonDocument::fromJson(file.readAll()).object();
    m_token = o.value("token").toString();
    if (o.contains("port")) {
        m_port = o.value("port").toInt(m_port);
    }
    return !m_token.isEmpty();
}

void
RemoteControlPanel::poll()
{
    if (!m_running) {
        return;
    }
    if (m_token.isEmpty() && !loadToken()) {
        m_reachable = false;
        updateUi();
        return;
    }
    QNetworkRequest request(QUrl(QString("http://127.0.0.1:%1/remote-control").arg(m_port)));
    request.setRawHeader("X-Etherwaver-Token", m_token.toUtf8());
    m_net.get(request);
}

void
RemoteControlPanel::post(const QString& pathAndQuery)
{
    if (m_token.isEmpty() && !loadToken()) {
        QMessageBox::warning(this, tr("Remote control"),
                             tr("The client is not running or its control interface is not available."));
        return;
    }
    QNetworkRequest request(QUrl(QString("http://127.0.0.1:%1%2").arg(m_port).arg(pathAndQuery)));
    request.setRawHeader("X-Etherwaver-Token", m_token.toUtf8());
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    m_net.post(request, QByteArray());
}

void
RemoteControlPanel::handleReply(QNetworkReply* reply)
{
    reply->deleteLater();
    const int code = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (code == 401) {
        // the client restarted with a new token
        m_token.clear();
        return;
    }
    if (reply->error() != QNetworkReply::NoError) {
        m_reachable = false;
        updateUi();
        return;
    }
    const QJsonObject o = QJsonDocument::fromJson(reply->readAll()).object();
    m_reachable = true;
    if (o.contains("status")) {
        m_status = o.value("status").toObject();
    }
    if (reply->operation() == QNetworkAccessManager::PostOperation && !o.value("ok").toBool(true)) {
        QMessageBox::warning(this, tr("Remote control"), o.value("error").toString());
    }
    updateUi();
}

bool
RemoteControlPanel::confirmHotkeyOnce()
{
    if (m_settings.value("remoteControl/hotkeyAcknowledged", false).toBool()) {
        return true;
    }
    QMessageBox box(this);
    box.setIcon(QMessageBox::Information);
    box.setWindowTitle(tr("Remote control"));
    box.setText(tr("The mouse and keyboard of this computer will control the server.\n\n"
                   "Press %1 at any time to return them to this computer. "
                   "The shortcut works even when the connection is lost.").arg(releaseHotkey()));
    QCheckBox* dontShow = new QCheckBox(tr("Do not show again"), &box);
    box.setCheckBox(dontShow);
    box.setStandardButtons(QMessageBox::Ok | QMessageBox::Cancel);
    if (box.exec() != QMessageBox::Ok) {
        return false;
    }
    if (dontShow->isChecked()) {
        m_settings.setValue("remoteControl/hotkeyAcknowledged", true);
    }
    return true;
}

void
RemoteControlPanel::start(bool takeover)
{
    if (!confirmHotkeyOnce()) {
        return;
    }
    QUrlQuery query;
    query.addQueryItem("mode", m_pModeCombo->currentData().toString());
    if (!m_pTargetEdit->text().trimmed().isEmpty()) {
        query.addQueryItem("target", m_pTargetEdit->text().trimmed());
    }
    if (takeover) {
        query.addQueryItem("takeover", "1");
    }
    post("/remote-control/start?" + query.toString(QUrl::FullyEncoded));
}

void RemoteControlPanel::startClicked()     { start(false); }
void RemoteControlPanel::takeoverClicked()  { start(true); }
void RemoteControlPanel::stopClicked()      { post("/remote-control/stop"); }
void RemoteControlPanel::ctrlAltDelClicked() { post("/remote-control/ctrl-alt-del"); }

QString
RemoteControlPanel::stateText(const QJsonObject& s)
{
    const QString state = s.value("state").toString();
    if (state == "remote") {
        return tr("Remote control");
    }
    if (state == "suspended") {
        return tr("Remote control (paused)");
    }
    if (state == "requesting") {
        return tr("Requesting control...");
    }
    if (s.value("viewOnly").toBool()) {
        return tr("View only");
    }
    return tr("Local control");
}

QString
RemoteControlPanel::eventText(const QString& code, const QJsonObject& s)
{
    const QString hotkey = s.value("releaseHotkey").toString();
    const QString target = s.value("target").toString().isEmpty()
                         ? tr("the server") : s.value("target").toString();
    if (code == "granted" || code == "focus" || code == "target-changed") {
        return tr("Mouse and keyboard now control: %1. Press %2 to return to this computer.")
            .arg(target, hotkey);
    }
    if (code == "focus-lost") {
        return tr("The session window lost the focus: forwarding is paused and all keys were released. "
                  "Click the window to continue.");
    }
    if (code == "connection-lost") {
        return tr("Connection to the server lost. Mouse and keyboard were returned to this computer; "
                  "the server releases all keys by itself.");
    }
    if (code == "hotkey") {
        return tr("%1 pressed: mouse and keyboard returned to this computer.").arg(hotkey);
    }
    if (code == "user") {
        return tr("Mouse and keyboard returned to this computer.");
    }
    if (code == "denied" && s.value("viewOnly").toBool()) {
        return tr("This computer may only be controlled. Allow it to control the server in the "
                  "server's screen settings (\"Allow this computer to control the server\").");
    }
    if (code == "denied") {
        return tr("Control refused: %1").arg(s.value("lastMessage").toString().section(": ", 1));
    }
    if (code == "revoked") {
        return tr("Control ended by the server: %1").arg(s.value("lastMessage").toString().section(": ", 1));
    }
    if (code == "suspend") {
        return tr("This computer is going to sleep or is locking: mouse and keyboard released.");
    }
    if (code == "capture-lost") {
        return tr("Capturing was interrupted by the system: mouse and keyboard returned to this computer.");
    }
    if (code == "queue-overflow") {
        return tr("The network could not keep up: remote control stopped and all keys were released.");
    }
    return s.value("lastMessage").toString();
}

void
RemoteControlPanel::updateUi()
{
    const QString state = m_status.value("state").toString("local");
    const bool active = (state == "remote" || state == "suspended" || state == "requesting");
    const bool connected = m_status.value("connected").toBool();
    const bool supports = m_status.value("serverSupports").toBool();
    const bool captureOk = m_status.value("captureAvailable").toBool();
    const QString controller = m_status.value("controller").toString();
    const bool otherControls = !active && !controller.isEmpty();

    m_pStateLabel->setText(m_running && m_reachable ? stateText(m_status) : tr("Local control"));

    QString info;
    QString warning;
    if (!m_running) {
        info = tr("Start the client to control the server from here.");
    }
    else if (!m_reachable) {
        info = tr("Waiting for the client...");
    }
    else {
        info = eventText(m_status.value("lastEvent").toString(), m_status);
        if (state == "remote" || state == "suspended") {
            info = eventText(state == "suspended" ? "focus-lost" : "granted", m_status);
        }
        if (otherControls && info.isEmpty()) {
            info = tr("Currently controlled by: %1").arg(controller);
        }
        else if (otherControls) {
            info += "\n" + tr("Currently controlled by: %1").arg(controller);
        }
        if (info.isEmpty()) {
            info = tr("Press \"Control with local mouse and keyboard\" to use this computer's devices "
                      "on the server. %1 always returns them here.").arg(releaseHotkey());
        }
        if (!connected) {
            warning = tr("Not connected to the server.");
        }
        else if (!supports) {
            warning = tr("The server does not support remote control. Update it to this Etherwaver version.");
        }
        else if (!captureOk) {
            warning = tr("Input capture is not available: %1")
                          .arg(m_status.value("captureUnavailableReason").toString());
        }
    }
    m_pInfoLabel->setText(info);
    m_pWarningLabel->setText(warning);
    m_pWarningLabel->setVisible(!warning.isEmpty());

    const bool canStart = m_running && m_reachable && connected && supports && captureOk && !active;
    m_pStartButton->setEnabled(canStart);
    m_pTakeoverButton->setVisible(canStart && otherControls);
    m_pStopButton->setEnabled(m_running && m_reachable && active);
    m_pCadButton->setEnabled(m_running && m_reachable && (state == "remote" || state == "suspended"));
    m_pCadButton->setToolTip(m_status.value("ctrlAltDelNative").toBool()
        ? tr("Sends the secure attention sequence to the controlled computer.")
        : tr("Sent as an ordinary key combination; the controlled system decides what it does."));
    m_pModeCombo->setEnabled(!active);
    m_pTargetEdit->setEnabled(!active);

    // offer only the modes the capture backend supports
    if (m_reachable) {
        const int window = m_pModeCombo->findData("window");
        const int capture = m_pModeCombo->findData("capture");
        const bool windowOk  = m_status.value("windowModeSupported").toBool(true);
        const bool captureModeOk = m_status.value("captureModeSupported").toBool(true);
        if (!windowOk && m_pModeCombo->currentIndex() == window) {
            m_pModeCombo->setCurrentIndex(capture);
        }
        if (!captureModeOk && m_pModeCombo->currentIndex() == capture) {
            m_pModeCombo->setCurrentIndex(window);
        }
    }
}
