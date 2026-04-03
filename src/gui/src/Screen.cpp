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

#include "Screen.h"

#include <QtCore>
#include <QtGui>

Screen::Screen() :
    m_Pixmap(QPixmap(":res/icons/64x64/video-display.png")),
    m_Swapped(false)
{
    init();
}

Screen::Screen(const QString& name) :
    m_Pixmap(QPixmap(":res/icons/64x64/video-display.png")),
    m_Swapped(false)
{
    init();
    setName(name);
}

void Screen::init()
{
    name().clear();
    aliases().clear();
    setPosition(QPoint(0, 0));
    setSize(QSize(240, 140));
    clearLinks();
    modifiers().clear();
    switchCorners().clear();
    fixes().clear();
    setSwitchCornerSize(0);

    // m_Modifiers, m_SwitchCorners and m_Fixes are QLists we use like fixed-size arrays,
    // thus we need to make sure to fill them with the required number of elements.
    for (int i = 0; i < static_cast<int>(Modifier::Count); i++)
        modifiers() << static_cast<Modifier>(i);

    for (int i = 0; i < static_cast<int>(SwitchCorner::Count); i++)
        switchCorners() << false;

    for (int i = 0; i < static_cast<int>(Fix::Count); i++)
        fixes() << false;
}

void Screen::loadSettings(QSettings& settings)
{
    setName(settings.value("name").toString());

    if (name().isEmpty())
        return;

    setSwitchCornerSize(settings.value("switchCornerSize").toInt());
    setPosition(QPoint(settings.value("layoutX", 0).toInt(),
                       settings.value("layoutY", 0).toInt()));
    setSize(QSize(settings.value("layoutWidth", 240).toInt(),
                  settings.value("layoutHeight", 140).toInt()));
    setLink(LinkRight, settings.value("linkRight").toString());
    setLink(LinkLeft, settings.value("linkLeft").toString());
    setLink(LinkUp, settings.value("linkUp").toString());
    setLink(LinkDown, settings.value("linkDown").toString());

    readSettings<QString>(settings, aliases(), "alias", QString(""));
    readSettings<int>(settings, modifiers(), "modifier", Modifier::DefaultMod,
                      static_cast<int>(Modifier::Count));
    readSettings<bool>(settings, switchCorners(), "switchCorner", false,
                       static_cast<int>(SwitchCorner::Count));
    readSettings<bool>(settings, fixes(), "fix", false, static_cast<int>(Fix::Count));
}

void Screen::saveSettings(QSettings& settings) const
{
    settings.setValue("name", name());

    if (name().isEmpty())
        return;

    settings.setValue("switchCornerSize", switchCornerSize());
    settings.setValue("layoutX", position().x());
    settings.setValue("layoutY", position().y());
    settings.setValue("layoutWidth", size().width());
    settings.setValue("layoutHeight", size().height());
    settings.setValue("linkRight", link(LinkRight));
    settings.setValue("linkLeft", link(LinkLeft));
    settings.setValue("linkUp", link(LinkUp));
    settings.setValue("linkDown", link(LinkDown));

    writeSettings<QString>(settings, aliases(), "alias");
    writeSettings<int>(settings, modifiers(), "modifier");
    writeSettings<bool>(settings, switchCorners(), "switchCorner");
    writeSettings<bool>(settings, fixes(), "fix");
}

QTextStream& Screen::writeScreensSection(QTextStream& outStream) const
{
    outStream << "\t" << name() << ":" << endl;

    for (int i = 0; i < modifiers().size(); i++) {
        auto mod = static_cast<Modifier>(i);
        if (modifier(mod) != mod) {
            outStream << "\t\t" << modifierName(mod) << " = " << modifierName(modifier(mod))
                      << endl;
        }
    }

    for (int i = 0; i < fixes().size(); i++) {
        auto fix = static_cast<Fix>(i);
        outStream << "\t\t" << fixName(fix) << " = " << (fixes()[i] ? "true" : "false") << endl;
    }

    outStream << "\t\t" << "switchCorners = none ";
    for (int i = 0; i < switchCorners().size(); i++) {
        if (switchCorners()[i]) {
            outStream << "+" << switchCornerName(static_cast<SwitchCorner>(i)) << " ";
        }
    }
    outStream << endl;

    outStream << "\t\t" << "switchCornerSize = " << switchCornerSize() << endl;

    return outStream;
}

QTextStream& Screen::writeAliasesSection(QTextStream& outStream) const
{
    if (!aliases().isEmpty())
    {
        outStream << "\t" << name() << ":" << endl;

        for (const QString& alias : aliases()) {
            outStream << "\t\t" << alias << endl;
        }
    }

    return outStream;
}

QDataStream& operator<<(QDataStream& outStream, const Screen& screen)
{
    QList<int> modifiers;
    for (auto mod : screen.modifiers()) {
        modifiers.push_back(static_cast<int>(mod));
    }

    return outStream
        << screen.name()
        << screen.position()
        << screen.size()
        << screen.links()
        << screen.switchCornerSize()
        << screen.aliases()
        << modifiers
        << screen.switchCorners()
        << screen.fixes()
        ;
}

QDataStream& operator>>(QDataStream& inStream, Screen& screen)
{
    QList<int> modifiers;
    inStream
        >> screen.m_Name
        >> screen.m_Position
        >> screen.m_Size
        >> screen.m_Links
        >> screen.m_SwitchCornerSize
        >> screen.m_Aliases
        >> modifiers
        >> screen.m_SwitchCorners
        >> screen.m_Fixes;

    screen.m_Modifiers.clear();
    for (auto mod : modifiers) {
        screen.m_Modifiers.push_back(static_cast<Screen::Modifier>(mod));
    }

    if (screen.m_Links.size() < static_cast<int>(Screen::LinkCount)) {
        while (screen.m_Links.size() < static_cast<int>(Screen::LinkCount)) {
            screen.m_Links << QString();
        }
    }

    return inStream;
}
