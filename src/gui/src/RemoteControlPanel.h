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

#pragma once

#include <QGroupBox>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QString>
#include <QTimer>

class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QSettings;
class QNetworkReply;

//! "Control the server with this computer's mouse and keyboard"
/*!
Talks to the running waver client through its local control API
(127.0.0.1, token from the profile directory) and shows whether the
keyboard and mouse currently work locally, control the server, or the
computer is allowed to view only.
*/
class RemoteControlPanel : public QGroupBox
{
    Q_OBJECT

public:
    RemoteControlPanel(QSettings& settings, const QString& profileDir, QWidget* parent = nullptr);

    //! the client process runs (poll the API) or not
    void setClientRunning(bool running);

    QString releaseHotkey() const;
    int apiPort() const { return m_port; }

    //! texts for the codes the client reports (unit tested)
    static QString stateText(const QJsonObject& status);
    static QString eventText(const QString& code, const QJsonObject& status);

private slots:
    void poll();
    void startClicked();
    void takeoverClicked();
    void stopClicked();
    void ctrlAltDelClicked();
    void hotkeyEdited();

private:
    void post(const QString& path);
    void handleReply(QNetworkReply* reply);
    bool loadToken();
    void updateUi();
    bool confirmHotkeyOnce();
    void start(bool takeover);

private:
    QSettings&      m_settings;
    QString         m_profileDir;
    QNetworkAccessManager m_net;
    QTimer          m_timer;
    QString         m_token;
    int             m_port;
    bool            m_running;
    bool            m_reachable;
    QJsonObject     m_status;

    QLabel*         m_pStateLabel;
    QLabel*         m_pInfoLabel;
    QLabel*         m_pWarningLabel;
    QComboBox*      m_pModeCombo;
    QLineEdit*      m_pTargetEdit;
    QLineEdit*      m_pHotkeyEdit;
    QPushButton*    m_pStartButton;
    QPushButton*    m_pTakeoverButton;
    QPushButton*    m_pStopButton;
    QPushButton*    m_pCadButton;
};
