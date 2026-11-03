/*
 * barrier -- mouse and keyboard sharing utility
 * Copyright (C) 2014-2016 Symless Ltd.
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

#pragma once

#include "barrier/ArgsBase.h"

class NetworkAddress;

class ClientArgs : public ArgsBase {
public:
    ClientArgs();

public:
    int                    m_yscroll;
    bool                m_uhidEnabled;
    String                m_uhidName;

    // remote control (forward this computer's keyboard and mouse)
    String              m_remoteControlReleaseHotkey; // empty = default
    int                 m_remoteControlApiPort;       // 0 = disabled
    String              m_remoteControlBackend;       // auto, x11, evdev, none
    String              m_remoteControlXkbLayout;     // evdev: keyboard layout
    String              m_remoteControlXkbVariant;    // evdev: layout variant
};
