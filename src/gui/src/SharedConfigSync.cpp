/*
 * Etherwaver -- keyboard and mouse sharing utility
 * Copyright (C) 2026 Etherneco Ltd.
 *
 * This package is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * found in the file LICENSE that should have accompanied this file.
 */

#include "SharedConfigSync.h"

#include <QCryptographicHash>
#include <QMessageBox>
#include <QPushButton>
#include <QSettings>
#include <QStringList>

namespace SharedConfigSync {

const char* kServerGroup = "internalConfig";

QByteArray digest(QSettings& settings, const QString& group)
{
    settings.sync();

    QStringList keys;
    if (group.isEmpty()) {
        keys = settings.childKeys();
    }
    else {
        settings.beginGroup(group);
        keys = settings.allKeys();
    }
    keys.sort();

    QCryptographicHash hash(QCryptographicHash::Sha256);
    for (const QString& key : keys) {
        hash.addData(key.toUtf8());
        hash.addData("=", 1);
        hash.addData(settings.value(key).toString().toUtf8());
        hash.addData("\n", 1);
    }

    if (!group.isEmpty()) {
        settings.endGroup();
    }
    return hash.result();
}

bool confirmOverwrite(QWidget* parent, const QString& what)
{
    QMessageBox box(parent);
    box.setIcon(QMessageBox::Warning);
    box.setWindowTitle(QObject::tr("Configuration changed elsewhere"));
    box.setText(QObject::tr("The %1 was changed by another panel (for example the "
                            "EtherWaver web panel) while this window was open.").arg(what));
    box.setInformativeText(QObject::tr("Overwrite those changes with yours, or discard your "
                                       "changes and load the current configuration?"));
    QPushButton* overwrite = box.addButton(QObject::tr("Overwrite"), QMessageBox::DestructiveRole);
    QPushButton* discard = box.addButton(QObject::tr("Discard my changes"), QMessageBox::AcceptRole);
    box.setDefaultButton(discard);
    box.exec();
    return box.clickedButton() == overwrite;
}

}
