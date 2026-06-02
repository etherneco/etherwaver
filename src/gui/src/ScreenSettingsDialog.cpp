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

#include "ScreenSettingsDialog.h"
#include "Screen.h"

#include <QtCore>
#include <QtGui>
#include <QMessageBox>
#include <QSignalBlocker>
#include <QDebug>

static const QRegExp ValidScreenName("[a-z0-9\\._-]{,255}", Qt::CaseInsensitive);

namespace {

bool splitTrailingScreenNumber(const QString& fullName, QString& baseName, int& number)
{
    QString candidate = fullName.trimmed();
    int parsedNumber = 0;
    bool foundSuffix = false;

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
        parsedNumber = suffixNumber;
        foundSuffix = true;
    }

    if (foundSuffix && !candidate.isEmpty()) {
        baseName = candidate;
        number = parsedNumber;
        return true;
    }

    return false;
}

void normalizeScreenIdentity(const QString& rawName, int rawNumber, QString& baseName, int& number)
{
    QString parsedBaseName;
    int parsedNumber = rawNumber;

    if (splitTrailingScreenNumber(rawName, parsedBaseName, parsedNumber)) {
        baseName = parsedBaseName;

        // If the UI spinbox is still on its default value, prefer the number
        // encoded in the name so editing an existing "host-N" screen does not
        // accidentally become "host-N-1".
        number = (rawNumber <= 1) ? parsedNumber : rawNumber;
        return;
    }

    baseName = rawName;
    number = (rawNumber > 0) ? rawNumber : 1;
}

} // namespace

static QString check_name_param(QString name)
{
    // after internationalization happens the default name "Unnamed" might
    // be translated with spaces (or other chars). let's replace the spaces
    // with dashes and just give up if that doesn't pass the regexp
    name.replace(' ', '-');
    if (ValidScreenName.exactMatch(name))
        return name;
    return "";
}

ScreenSettingsDialog::ScreenSettingsDialog(QWidget* parent, Screen* pScreen) :
    QDialog(parent, Qt::WindowTitleHint | Qt::WindowSystemMenuHint),
    Ui::ScreenSettingsDialogBase(),
    m_pScreen(pScreen)
{
    setupUi(this);

    qDebug() << "ScreenSettingsDialog ctor raw screen name:" << m_pScreen->name();

    m_pLineEditName->setValidator(new QRegExpValidator(ValidScreenName, m_pLineEditName));
    normalizeNameWidgets(m_pScreen->name(), 1);
    m_pLineEditName->selectAll();

    m_pLineEditAlias->setValidator(new QRegExpValidator(ValidScreenName, m_pLineEditName));

    for (int i = 0; i < m_pScreen->aliases().count(); i++)
        new QListWidgetItem(m_pScreen->aliases()[i], m_pListAliases);

    m_pComboBoxShift->setCurrentIndex(static_cast<int>(m_pScreen->modifier(Screen::Modifier::Shift)));
    m_pComboBoxCtrl->setCurrentIndex(static_cast<int>(m_pScreen->modifier(Screen::Modifier::Ctrl)));
    m_pComboBoxAlt->setCurrentIndex(static_cast<int>(m_pScreen->modifier(Screen::Modifier::Alt)));
    m_pComboBoxMeta->setCurrentIndex(static_cast<int>(m_pScreen->modifier(Screen::Modifier::Meta)));
    m_pComboBoxSuper->setCurrentIndex(static_cast<int>(m_pScreen->modifier(Screen::Modifier::Super)));

    m_pCheckBoxCornerTopLeft->setChecked(m_pScreen->switchCorner(Screen::SwitchCorner::TopLeft));
    m_pCheckBoxCornerTopRight->setChecked(m_pScreen->switchCorner(Screen::SwitchCorner::TopRight));
    m_pCheckBoxCornerBottomLeft->setChecked(m_pScreen->switchCorner(Screen::SwitchCorner::BottomLeft));
    m_pCheckBoxCornerBottomRight->setChecked(m_pScreen->switchCorner(Screen::SwitchCorner::BottomRight));
    m_pSpinBoxSwitchCornerSize->setValue(m_pScreen->switchCornerSize());

    m_pCheckBoxCapsLock->setChecked(m_pScreen->fix(Screen::Fix::CapsLock));
    m_pCheckBoxNumLock->setChecked(m_pScreen->fix(Screen::Fix::NumLock));
    m_pCheckBoxScrollLock->setChecked(m_pScreen->fix(Screen::Fix::ScrollLock));
    m_pCheckBoxXTest->setChecked(m_pScreen->fix(Screen::Fix::XTest));
    m_pCheckBoxPreserveFocus->setChecked(m_pScreen->fix(Screen::Fix::PreserveFocus));
}

void ScreenSettingsDialog::accept()
{
    if (m_pLineEditName->text().isEmpty())
    {
        QMessageBox::warning(
            this, tr("Screen name is empty"),
            tr("The screen name cannot be empty. "
               "Please either fill in a name or cancel the dialog."));
        return;
    }

    normalizeNameWidgets(m_pLineEditName->text(), m_pSpinBoxNumber->value());

    m_pScreen->init();

    const QString fullName = composedScreenName();
    m_pScreen->setName(fullName);

    for (int i = 0; i < m_pListAliases->count(); i++)
    {
        QString alias(m_pListAliases->item(i)->text());
        if (alias == fullName)
        {
            QMessageBox::warning(
                this, tr("Screen name matches alias"),
                tr("The screen name cannot be the same as an alias. "
                   "Please either remove the alias or change the screen name."));
            return;
        }
        m_pScreen->addAlias(alias);
    }

    m_pScreen->setModifier(Screen::Modifier::Shift,
                           static_cast<Screen::Modifier>(m_pComboBoxShift->currentIndex()));
    m_pScreen->setModifier(Screen::Modifier::Ctrl,
                           static_cast<Screen::Modifier>(m_pComboBoxCtrl->currentIndex()));
    m_pScreen->setModifier(Screen::Modifier::Alt,
                           static_cast<Screen::Modifier>(m_pComboBoxAlt->currentIndex()));
    m_pScreen->setModifier(Screen::Modifier::Meta,
                           static_cast<Screen::Modifier>(m_pComboBoxMeta->currentIndex()));
    m_pScreen->setModifier(Screen::Modifier::Super,
                           static_cast<Screen::Modifier>(m_pComboBoxSuper->currentIndex()));

    m_pScreen->setSwitchCorner(Screen::SwitchCorner::TopLeft, m_pCheckBoxCornerTopLeft->isChecked());
    m_pScreen->setSwitchCorner(Screen::SwitchCorner::TopRight, m_pCheckBoxCornerTopRight->isChecked());
    m_pScreen->setSwitchCorner(Screen::SwitchCorner::BottomLeft, m_pCheckBoxCornerBottomLeft->isChecked());
    m_pScreen->setSwitchCorner(Screen::SwitchCorner::BottomRight, m_pCheckBoxCornerBottomRight->isChecked());
    m_pScreen->setSwitchCornerSize(m_pSpinBoxSwitchCornerSize->value());

    m_pScreen->setFix(Screen::Fix::CapsLock, m_pCheckBoxCapsLock->isChecked());
    m_pScreen->setFix(Screen::Fix::NumLock, m_pCheckBoxNumLock->isChecked());
    m_pScreen->setFix(Screen::Fix::ScrollLock, m_pCheckBoxScrollLock->isChecked());
    m_pScreen->setFix(Screen::Fix::XTest, m_pCheckBoxXTest->isChecked());
    m_pScreen->setFix(Screen::Fix::PreserveFocus, m_pCheckBoxPreserveFocus->isChecked());

    QDialog::accept();
}

void ScreenSettingsDialog::on_m_pButtonAddAlias_clicked()
{
    if (!m_pLineEditAlias->text().isEmpty() && m_pListAliases->findItems(m_pLineEditAlias->text(), Qt::MatchFixedString).isEmpty())
    {
        new QListWidgetItem(m_pLineEditAlias->text(), m_pListAliases);
        m_pLineEditAlias->clear();
    }
}

void ScreenSettingsDialog::on_m_pLineEditAlias_textChanged(const QString& text)
{
    m_pButtonAddAlias->setEnabled(!text.isEmpty());
}

void ScreenSettingsDialog::on_m_pLineEditName_textChanged(const QString& text)
{
    normalizeNameWidgets(text, m_pSpinBoxNumber->value());
}

void ScreenSettingsDialog::on_m_pButtonRemoveAlias_clicked()
{
    QList<QListWidgetItem*> items = m_pListAliases->selectedItems();

    for (int i = 0; i < items.count(); i++)
        delete items[i];
}

void ScreenSettingsDialog::on_m_pListAliases_itemSelectionChanged()
{
    m_pButtonRemoveAlias->setEnabled(!m_pListAliases->selectedItems().isEmpty());
}

QString ScreenSettingsDialog::composedScreenName() const
{
    QString normalizedBaseName;
    int normalizedNumber = m_pSpinBoxNumber->value();
    normalizeScreenIdentity(m_pLineEditName->text(), normalizedNumber, normalizedBaseName, normalizedNumber);

    return QString("%1-%2")
        .arg(check_name_param(normalizedBaseName))
        .arg(normalizedNumber);
}

void ScreenSettingsDialog::splitScreenName(const QString& fullName, QString& baseName, int& number) const
{
    normalizeScreenIdentity(fullName, number, baseName, number);
}

void ScreenSettingsDialog::normalizeNameWidgets(const QString& rawName, int rawNumber)
{
    QString normalizedBaseName;
    int normalizedNumber = rawNumber;
    normalizeScreenIdentity(rawName, normalizedNumber, normalizedBaseName, normalizedNumber);

    qDebug() << "normalizeNameWidgets rawName=" << rawName
             << "rawNumber=" << rawNumber
             << "normalizedBaseName=" << normalizedBaseName
             << "normalizedNumber=" << normalizedNumber;

    const QString sanitizedBaseName = check_name_param(normalizedBaseName);
    const bool needsNameUpdate = (m_pLineEditName->text() != sanitizedBaseName);
    const bool needsNumberUpdate = (m_pSpinBoxNumber->value() != normalizedNumber);

    if (!needsNameUpdate && !needsNumberUpdate) {
        return;
    }

    const QSignalBlocker nameBlocker(m_pLineEditName);
    const QSignalBlocker numberBlocker(m_pSpinBoxNumber);

    if (needsNameUpdate) {
        m_pLineEditName->setText(sanitizedBaseName);
    }
    if (needsNumberUpdate) {
        m_pSpinBoxNumber->setValue(normalizedNumber);
    }
}
