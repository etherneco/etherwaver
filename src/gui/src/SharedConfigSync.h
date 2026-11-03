/*
 * Etherwaver -- keyboard and mouse sharing utility
 * Copyright (C) 2026 Etherneco Ltd.
 *
 * This package is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * found in the file LICENSE that should have accompanied this file.
 */

#pragma once

#include <QByteArray>
#include <QString>

class QSettings;
class QWidget;

/*
 * The desktop panel and the web panel (waver-web) edit the same QSettings
 * file.  These helpers let the desktop panel notice changes made by the
 * other panel and refuse to silently overwrite them.
 */
namespace SharedConfigSync {

//! Group holding the server layout (ServerConfig).
extern const char* kServerGroup;

/*!
 Re-reads the settings file from disk and returns a digest of the values
 below \p group ("" means the top-level keys used by AppConfig).
*/
QByteArray digest(QSettings& settings, const QString& group);

/*!
 Asks the user what to do when \p what was changed by another panel while
 a dialog was open.  Returns true to overwrite the other change, false to
 discard the local edit.
*/
bool confirmOverwrite(QWidget* parent, const QString& what);

}
