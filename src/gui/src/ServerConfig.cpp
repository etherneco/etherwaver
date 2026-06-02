/*
 * barrier -- mouse and keyboard sharing utility
 * Copyright (C) 2012-2016 Symless Ltd.
 * Copyright (C) 2008 Volker Lanz (vl@fidra.de)
 *
 * This package is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * found in the file LICENSE that should have accompanied this file.
 *
 * This package is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "ServerConfig.h"
#include "Hotkey.h"
#include "MainWindow.h"
#include "AddClientDialog.h"

#include <QtCore>
#include <QMessageBox>
#include <QAbstractButton>
#include <QPushButton>

static const struct
{
     int x;
     int y;
     const char* name;
} neighbourDirs[] =
{
    {  1,  0, "right" },
    { -1,  0, "left" },
    {  0, -1, "up" },
    {  0,  1, "down" },

};

namespace {

bool splitTrailingScreenNumber(const QString& screenName, QString& baseName, int& number)
{
    const QString candidate = screenName.trimmed();
    const int dashPos = candidate.lastIndexOf('-');
    if (dashPos <= 0 || dashPos + 1 >= candidate.length()) {
        return false;
    }

    bool ok = false;
    const int parsedNumber = candidate.mid(dashPos + 1).toInt(&ok);
    if (!ok || parsedNumber <= 0) {
        return false;
    }

    baseName = candidate.left(dashPos);
    number = parsedNumber;
    return !baseName.isEmpty();
}

QString ensureScreenNumberSuffix(const QString& screenName)
{
    const QString trimmed = screenName.trimmed();
    if (trimmed.isEmpty()) {
        return trimmed;
    }

    QString baseName;
    int number = 0;
    if (splitTrailingScreenNumber(trimmed, baseName, number)) {
        return trimmed;
    }

    return trimmed + "-1";
}

QString baseHostName(const QString& screenName)
{
    QString candidate = screenName.trimmed();

    while (!candidate.isEmpty()) {
        const int dashPos = candidate.lastIndexOf('-');
        if (dashPos <= 0 || dashPos + 1 >= candidate.length()) {
            break;
        }

        bool ok = false;
        const int suffixNumber = candidate.mid(dashPos + 1).toInt(&ok);
        if (!ok || suffixNumber <= 0) {
            break;
        }

        candidate = candidate.left(dashPos);
    }

    return candidate.isEmpty() ? screenName : candidate;
}

void normalizeScreenNames(std::vector<Screen>& screens)
{
    QMap<QString, QString> renamedScreens;

    for (std::vector<Screen>::iterator it = screens.begin(); it != screens.end(); ++it) {
        if (it->isNull()) {
            continue;
        }

        const QString oldName = it->name();
        const QString normalizedName = ensureScreenNumberSuffix(oldName);
        if (oldName.compare(normalizedName, Qt::CaseInsensitive) != 0) {
            renamedScreens.insert(oldName.toLower(), normalizedName);
            it->setName(normalizedName);
        }
    }

    for (std::vector<Screen>::iterator it = screens.begin(); it != screens.end(); ++it) {
        if (it->isNull()) {
            continue;
        }

        const Screen::LinkDirection directions[] = {
            Screen::LinkRight, Screen::LinkLeft, Screen::LinkUp, Screen::LinkDown
        };
        for (unsigned int i = 0; i < sizeof(directions) / sizeof(directions[0]); ++i) {
            const QString target = it->link(directions[i]).trimmed();
            if (target.isEmpty()) {
                continue;
            }

            QMap<QString, QString>::const_iterator renamed =
                renamedScreens.find(target.toLower());
            if (renamed != renamedScreens.end()) {
                it->setLink(directions[i], renamed.value());
            }
            else {
                it->setLink(directions[i], ensureScreenNumberSuffix(target));
            }
        }
    }
}

} // namespace

const int serverDefaultIndex = 7;

ServerConfig::ServerConfig(QSettings* settings, int numColumns, int numRows ,
                QString serverName, MainWindow* mainWindow) :
    m_pSettings(settings),
    m_Screens(),
    m_NumColumns(numColumns),
    m_NumRows(numRows),
    m_UseCustomLinks(false),
    m_ServerName(serverName),
    m_IgnoreAutoConfigClient(false),
    m_EnableDragAndDrop(false),
    m_ClipboardSharing(true),
    m_pMainWindow(mainWindow)
{
    Q_ASSERT(m_pSettings);

    loadSettings();
}

ServerConfig::~ServerConfig()
{
    saveSettings();
}

bool ServerConfig::save(const QString& fileName) const
{
    QFile file(fileName);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text))
        return false;

    save(file);
    file.close();

    return true;
}

bool ServerConfig::saveLayout(const QString& configFileName) const
{
    QFileInfo configInfo(configFileName);
    const QString layoutPath =
        configInfo.absoluteDir().absoluteFilePath("etherwaver-layout.json");

    QSaveFile file(layoutPath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        return false;
    }

    QTextStream out(&file);
    out << "{\n  \"screens\": [\n";

    bool first = true;
    for (const Screen& screen : screens()) {
        if (screen.isNull()) {
            continue;
        }

        if (!first) {
            out << ",\n";
        }
        first = false;

        // The host field should contain the machine hostname without any
        // trailing screen-number suffixes.
        QString hostName = baseHostName(screen.name());

        const QSize size = screen.size().isValid() ? screen.size() : QSize(240, 140);
        out << "    {\n"
            << "      \"id\": \"" << screen.name() << "\",\n"
            << "      \"host\": \"" << hostName << "\",\n"
            << "      \"name\": \"" << screen.name() << "\",\n"
            << "      \"x\": " << screen.position().x() << ",\n"
            << "      \"y\": " << screen.position().y() << ",\n"
            << "      \"width\": " << size.width() << ",\n"
            << "      \"height\": " << size.height();

        if (useCustomLinks()) {
            out << ",\n"
                << "      \"links\": {\n"
                << "        \"right\": \"" << screen.link(Screen::LinkRight) << "\",\n"
                << "        \"left\": \"" << screen.link(Screen::LinkLeft) << "\",\n"
                << "        \"up\": \"" << screen.link(Screen::LinkUp) << "\",\n"
                << "        \"down\": \"" << screen.link(Screen::LinkDown) << "\"\n"
                << "      }\n"
                << "    }";
        }
        else {
            out << "\n"
                << "    }";
        }
    }

    out << "\n  ]\n}\n";
    out.flush();

    return file.commit();
}

void ServerConfig::save(QFile& file) const
{
    QTextStream outStream(&file);
    outStream << *this;
}

void ServerConfig::init()
{
    switchCorners().clear();
    screens().clear();

    // m_NumSwitchCorners is used as a fixed size array. See Screen::init()
    for (int i = 0; i < static_cast<int>(SwitchCorner::Count); i++) {
        switchCorners() << false;
    }

    // There must always be screen objects for each cell in the screens QList. Unused screens
    // are identified by having an empty name.
    for (int i = 0; i < numColumns() * numRows(); i++)
        addScreen(Screen());
}

void ServerConfig::saveSettings()
{
    settings().beginGroup("internalConfig");
    settings().remove("");

    settings().setValue("numColumns", numColumns());
    settings().setValue("numRows", numRows());
    settings().setValue("useCustomLinks", useCustomLinks());

    settings().setValue("hasHeartbeat", hasHeartbeat());
    settings().setValue("heartbeat", heartbeat());
    settings().setValue("relativeMouseMoves", relativeMouseMoves());
    settings().setValue("screenSaverSync", screenSaverSync());
    settings().setValue("win32KeepForeground", win32KeepForeground());
    settings().setValue("hasSwitchDelay", hasSwitchDelay());
    settings().setValue("switchDelay", switchDelay());
    settings().setValue("hasSwitchDoubleTap", hasSwitchDoubleTap());
    settings().setValue("switchDoubleTap", switchDoubleTap());
    settings().setValue("switchCornerSize", switchCornerSize());
    settings().setValue("ignoreAutoConfigClient", ignoreAutoConfigClient());
    settings().setValue("enableDragAndDrop", enableDragAndDrop());
    settings().setValue("clipboardSharing", clipboardSharing());

    writeSettings<bool>(settings(), switchCorners(), "switchCorner");

    settings().beginWriteArray("screens");
    for (int i = 0; i < screens().size(); i++)
    {
        settings().setArrayIndex(i);
        screens()[i].saveSettings(settings());
    }
    settings().endArray();

    settings().beginWriteArray("hotkeys");
    for (int i = 0; i < hotkeys().size(); i++)
    {
        settings().setArrayIndex(i);
        hotkeys()[i].saveSettings(settings());
    }
    settings().endArray();

    settings().endGroup();
    settings().sync();
}

void ServerConfig::loadSettings()
{
    settings().beginGroup("internalConfig");

    setNumColumns(settings().value("numColumns", 5).toInt());
    setNumRows(settings().value("numRows", 3).toInt());
    setUseCustomLinks(settings().value("useCustomLinks", false).toBool());

    // we need to know the number of columns and rows before we can set up ourselves
    init();

    haveHeartbeat(settings().value("hasHeartbeat", false).toBool());
    setHeartbeat(settings().value("heartbeat", 5000).toInt());
    setRelativeMouseMoves(settings().value("relativeMouseMoves", false).toBool());
    setScreenSaverSync(settings().value("screenSaverSync", true).toBool());
    setWin32KeepForeground(settings().value("win32KeepForeground", false).toBool());
    haveSwitchDelay(settings().value("hasSwitchDelay", false).toBool());
    setSwitchDelay(settings().value("switchDelay", 250).toInt());
    haveSwitchDoubleTap(settings().value("hasSwitchDoubleTap", false).toBool());
    setSwitchDoubleTap(settings().value("switchDoubleTap", 250).toInt());
    setSwitchCornerSize(settings().value("switchCornerSize").toInt());
    setIgnoreAutoConfigClient(settings().value("ignoreAutoConfigClient").toBool());
    setEnableDragAndDrop(settings().value("enableDragAndDrop", true).toBool());
    setClipboardSharing(settings().value("clipboardSharing", true).toBool());

    readSettings<bool>(settings(), switchCorners(), "switchCorner", false,
                       static_cast<int>(SwitchCorner::Count));

    int numScreens = settings().beginReadArray("screens");
    Q_ASSERT(numScreens <= screens().size());
    for (int i = 0; i < numScreens; i++)
    {
        settings().setArrayIndex(i);
        screens()[i].loadSettings(settings());
    }
    settings().endArray();
    normalizeScreenNames(screens());

    int numHotkeys = settings().beginReadArray("hotkeys");
    for (int i = 0; i < numHotkeys; i++)
    {
        settings().setArrayIndex(i);
        Hotkey h;
        h.loadSettings(settings());
        hotkeys().push_back(h);
    }
    settings().endArray();

    settings().endGroup();

    // ensure the server's own screen is always present in the config
    m_ServerName = ensureScreenNumberSuffix(m_ServerName);
    int serverIndex = -1;
    if (!m_ServerName.isEmpty() && !findScreenName(m_ServerName, serverIndex)) {
        fixNoServer(m_ServerName, serverIndex);
    }
}

int ServerConfig::adjacentScreenIndex(int idx, int deltaColumn, int deltaRow) const
{
    if (screens()[idx].isNull())
        return -1;

    // if we're at the left or right end of the table, don't find results going further left or right
    if ((deltaColumn > 0 && (idx+1) % numColumns() == 0)
            || (deltaColumn < 0 && idx % numColumns() == 0))
        return -1;

    int arrayPos = idx + deltaColumn + deltaRow * numColumns();

    if (arrayPos >= screens().size() || arrayPos < 0)
        return -1;

    return arrayPos;
}

QTextStream& operator<<(QTextStream& outStream, const ServerConfig& config)
{
    outStream << "section: screens" << endl;

    for (const Screen& s : config.screens()) {
        if (!s.isNull())
            s.writeScreensSection(outStream);
    }

    outStream << "end" << endl << endl;

    outStream << "section: aliases" << endl;

    for (const Screen& s : config.screens()) {
        if (!s.isNull())
            s.writeAliasesSection(outStream);
    }

    outStream << "end" << endl << endl;

    outStream << "section: links" << endl;

    const auto screenExists = [&config](const QString& name) {
        if (name.isEmpty()) {
            return false;
        }
        for (const Screen& screen : config.screens()) {
            if (!screen.isNull() && screen.name().compare(name, Qt::CaseInsensitive) == 0) {
                return true;
            }
        }
        return false;
    };

    for (int i = 0; i < config.screens().size(); i++)
        if (!config.screens()[i].isNull())
        {
            outStream << "\t" << config.screens()[i].name() << ":" << endl;

            if (config.useCustomLinks()) {
                const Screen& screen = config.screens()[i];
                const Screen::LinkDirection directions[] = {
                    Screen::LinkRight, Screen::LinkLeft, Screen::LinkUp, Screen::LinkDown
                };
                for (unsigned int j = 0; j < sizeof(directions) / sizeof(directions[0]); ++j) {
                    const QString target = screen.link(directions[j]);
                    if (screenExists(target)) {
                        outStream << "\t\t" << neighbourDirs[j].name << " = " << target << endl;
                    }
                }
            }
            else {
                for (unsigned int j = 0; j < sizeof(neighbourDirs) / sizeof(neighbourDirs[0]); j++)
                {
                    int idx = config.adjacentScreenIndex(i, neighbourDirs[j].x, neighbourDirs[j].y);
                    if (idx != -1 && !config.screens()[idx].isNull())
                        outStream << "\t\t" << neighbourDirs[j].name << " = " << config.screens()[idx].name() << endl;
                }
            }
        }

    outStream << "end" << endl << endl;

    outStream << "section: options" << endl;

    if (config.hasHeartbeat())
        outStream << "\t" << "heartbeat = " << config.heartbeat() << endl;

    outStream << "\t" << "relativeMouseMoves = " << (config.relativeMouseMoves() ? "true" : "false") << endl;
    outStream << "\t" << "screenSaverSync = " << (config.screenSaverSync() ? "true" : "false") << endl;
    outStream << "\t" << "win32KeepForeground = " << (config.win32KeepForeground() ? "true" : "false") << endl;
    outStream << "\t" << "clipboardSharing = " << (config.clipboardSharing() ? "true" : "false") << endl;

    if (config.hasSwitchDelay())
        outStream << "\t" << "switchDelay = " << config.switchDelay() << endl;

    if (config.hasSwitchDoubleTap())
        outStream << "\t" << "switchDoubleTap = " << config.switchDoubleTap() << endl;

    outStream << "\t" << "switchCorners = none ";
    for (int i = 0; i < config.switchCorners().size(); i++) {
        auto corner = static_cast<Screen::SwitchCorner>(i);
        if (config.switchCorners()[i]) {
            outStream << "+" << config.switchCornerName(corner) << " ";
        }
    }
    outStream << endl;

    outStream << "\t" << "switchCornerSize = " << config.switchCornerSize() << endl;

    for (const Hotkey& hotkey : config.hotkeys()) {
        outStream << hotkey;
    }

    outStream << "end" << endl << endl;

    return outStream;
}

int ServerConfig::numScreens() const
{
    int rval = 0;

    for (const Screen& s : screens()) {
        if (!s.isNull())
            rval++;
    }

    return rval;
}

int ServerConfig::autoAddScreen(const QString name)
{
    const QString normalizedName = ensureScreenNumberSuffix(name);
    int serverIndex = -1;
    int targetIndex = -1;
    if (!findScreenName(m_ServerName, serverIndex)) {
        if (!fixNoServer(m_ServerName, serverIndex)) {
            return kAutoAddScreenManualServer;
        }
    }
    if (findScreenName(normalizedName, targetIndex)) {
        // already exists.
        return kAutoAddScreenIgnore;
    }

    int result = showAddClientDialog(normalizedName);

    if (result == kAddClientIgnore) {
        return kAutoAddScreenIgnore;
    }

    if (result == kAddClientOther) {
        addToFirstEmptyGrid(normalizedName);
        return kAutoAddScreenManualClient;
    }

    bool success = false;
    int startIndex = serverIndex;
    int offset = 1;
    int dirIndex = 0;

    if (result == kAddClientLeft) {
        offset = -1;
        dirIndex = 1;
    }
    else if (result == kAddClientUp) {
        offset = -5;
        dirIndex = 2;
    }
    else if (result == kAddClientDown) {
        offset = 5;
        dirIndex = 3;
    }


    int idx = adjacentScreenIndex(startIndex, neighbourDirs[dirIndex].x,
                    neighbourDirs[dirIndex].y);
    while (idx != -1) {
        if (screens()[idx].isNull()) {
            m_Screens[idx].setName(normalizedName);
            success = true;
            break;
        }

        startIndex += offset;
        idx = adjacentScreenIndex(startIndex, neighbourDirs[dirIndex].x,
                    neighbourDirs[dirIndex].y);
    }

    if (!success) {
        addToFirstEmptyGrid(normalizedName);
        return kAutoAddScreenManualClient;
    }

    saveSettings();
    return kAutoAddScreenOk;
}

bool ServerConfig::findScreenName(const QString& name, int& index)
{
    const QString normalizedName = ensureScreenNumberSuffix(name);
    bool found = false;
    for (int i = 0; i < screens().size(); i++) {
        if (!screens()[i].isNull() &&
            screens()[i].name().compare(normalizedName, Qt::CaseInsensitive) == 0) {
            index = i;
            found = true;
            break;
        }
    }
    return found;
}

bool ServerConfig::fixNoServer(const QString& name, int& index)
{
    bool fixed = false;
    const QString normalizedName = ensureScreenNumberSuffix(name);
    if (screens()[serverDefaultIndex].isNull()) {
        m_Screens[serverDefaultIndex].setName(normalizedName);
        index = serverDefaultIndex;
        fixed = true;
    }

    return fixed;
}

int ServerConfig::showAddClientDialog(const QString& clientName)
{
    int result = kAddClientIgnore;

    if (!m_pMainWindow->isActiveWindow()) {
        m_pMainWindow->showNormal();
        m_pMainWindow->activateWindow();
    }

    AddClientDialog addClientDialog(ensureScreenNumberSuffix(clientName), m_pMainWindow);
    addClientDialog.exec();
    result = addClientDialog.addResult();
    m_IgnoreAutoConfigClient = addClientDialog.ignoreAutoConfigClient();

    return result;
}

void::ServerConfig::addToFirstEmptyGrid(const QString &clientName)
{
    const QString normalizedName = ensureScreenNumberSuffix(clientName);
    for (int i = 0; i < screens().size(); i++) {
        if (screens()[i].isNull()) {
            m_Screens[i].setName(normalizedName);
            break;
        }
    }
}
