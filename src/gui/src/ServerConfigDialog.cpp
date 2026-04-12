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

#include "ServerConfigDialog.h"
#include "ServerConfig.h"
#include "HotkeyDialog.h"
#include "ActionDialog.h"
#include "LayoutEditorWidget.h"

#include <QtCore>
#include <QtGui>
#include <QMessageBox>
#include <QHBoxLayout>
#include <QPushButton>

namespace {

QString ensureScreenNumberSuffix(const QString& screenName)
{
    const QString trimmed = screenName.trimmed();
    if (trimmed.isEmpty()) {
        return trimmed;
    }

    const int dashPos = trimmed.lastIndexOf('-');
    if (dashPos > 0 && dashPos + 1 < trimmed.length()) {
        bool ok = false;
        const int number = trimmed.mid(dashPos + 1).toInt(&ok);
        if (ok && number > 0) {
            return trimmed;
        }
    }

    return trimmed + "-1";
}

} // namespace

ServerConfigDialog::ServerConfigDialog(QWidget* parent, ServerConfig& config, const QString& defaultScreenName) :
    QDialog(parent, Qt::WindowTitleHint | Qt::WindowSystemMenuHint),
    Ui::ServerConfigDialogBase(),
    m_OrigServerConfig(config),
    m_ServerConfig(config),
    m_layoutEditor(NULL),
    m_Message("")
{
    setupUi(this);

    m_pCheckBoxHeartbeat->setChecked(serverConfig().hasHeartbeat());
    m_pSpinBoxHeartbeat->setValue(serverConfig().heartbeat());

    m_pCheckBoxRelativeMouseMoves->setChecked(serverConfig().relativeMouseMoves());
    m_pCheckBoxScreenSaverSync->setChecked(serverConfig().screenSaverSync());
    m_pCheckBoxWin32KeepForeground->setChecked(serverConfig().win32KeepForeground());

    m_pCheckBoxSwitchDelay->setChecked(serverConfig().hasSwitchDelay());
    m_pSpinBoxSwitchDelay->setValue(serverConfig().switchDelay());

    m_pCheckBoxSwitchDoubleTap->setChecked(serverConfig().hasSwitchDoubleTap());
    m_pSpinBoxSwitchDoubleTap->setValue(serverConfig().switchDoubleTap());

    m_pCheckBoxCornerTopLeft->setChecked(serverConfig().switchCorner(BaseConfig::SwitchCorner::TopLeft));
    m_pCheckBoxCornerTopRight->setChecked(serverConfig().switchCorner(BaseConfig::SwitchCorner::TopRight));
    m_pCheckBoxCornerBottomLeft->setChecked(serverConfig().switchCorner(BaseConfig::SwitchCorner::BottomLeft));
    m_pCheckBoxCornerBottomRight->setChecked(serverConfig().switchCorner(BaseConfig::SwitchCorner::BottomRight));
    m_pSpinBoxSwitchCornerSize->setValue(serverConfig().switchCornerSize());

    m_pCheckBoxIgnoreAutoConfigClient->setChecked(serverConfig().ignoreAutoConfigClient());

    m_pCheckBoxEnableDragAndDrop->setChecked(serverConfig().enableDragAndDrop());

    m_pCheckBoxEnableClipboard->setChecked(serverConfig().clipboardSharing());

    for (const Hotkey& hotkey : serverConfig().hotkeys()) {
        m_pListHotkeys->addItem(hotkey.text());
    }

    if (serverConfig().numScreens() == 0 && !serverConfig().screens().empty()) {
        serverConfig().screens()[0] = Screen(ensureScreenNumberSuffix(defaultScreenName));
    }

    m_pTrashScreenWidget->hide();
    m_pLabelNewScreenWidget->hide();

    QPushButton* addButton = new QPushButton(tr("Add Screen"), this);
    QPushButton* editButton = new QPushButton(tr("Edit Selected"), this);
    QPushButton* removeButton = new QPushButton(tr("Delete Selected"), this);
    QPushButton* autoButton = new QPushButton(tr("Auto Layout"), this);
    QPushButton* zoomOutButton = new QPushButton(tr("-"), this);
    QPushButton* zoomInButton = new QPushButton(tr("+"), this);
    editButton->setEnabled(false);
    removeButton->setEnabled(false);
    zoomOutButton->setFixedWidth(32);
    zoomInButton->setFixedWidth(32);

    QHBoxLayout* toolbar = new QHBoxLayout();
    toolbar->addWidget(addButton);
    toolbar->addWidget(editButton);
    toolbar->addWidget(removeButton);
    toolbar->addWidget(autoButton);
    toolbar->addStretch();
    toolbar->addWidget(zoomOutButton);
    toolbar->addWidget(zoomInButton);
    m_pTabScreens->layout()->addItem(toolbar);

    m_layoutEditor = new LayoutEditorWidget(serverConfig(), this);
    m_pTabScreens->layout()->replaceWidget(m_pScreenSetupView, m_layoutEditor);
    m_layoutEditor->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    m_pScreenSetupView->hide();
    QVBoxLayout* screensLayout = qobject_cast<QVBoxLayout*>(m_pTabScreens->layout());
    if (screensLayout != NULL) {
        screensLayout->setStretch(1, 1);
    }

    connect(addButton, SIGNAL(clicked()), this, SLOT(onAddScreen()));
    connect(editButton, SIGNAL(clicked()), this, SLOT(onEditScreen()));
    connect(removeButton, SIGNAL(clicked()), this, SLOT(onRemoveScreen()));
    connect(autoButton, SIGNAL(clicked()), this, SLOT(onAutoLayout()));
    connect(zoomOutButton, SIGNAL(clicked()), m_layoutEditor, SLOT(zoomOut()));
    connect(zoomInButton, SIGNAL(clicked()), m_layoutEditor, SLOT(zoomIn()));
    connect(m_layoutEditor, SIGNAL(selectionChanged(bool)), editButton, SLOT(setEnabled(bool)));
    connect(m_layoutEditor, SIGNAL(selectionChanged(bool)), removeButton, SLOT(setEnabled(bool)));

    label_2->setText(tr("Arrange screens freely on the canvas, like a visual layout editor."));
    label_3->setText(tr("Drag screens freely, double click to edit, and save to generate EtherWaver object layout."));
}

void ServerConfigDialog::showEvent(QShowEvent* event)
{
    QDialog::show();

    if (!m_Message.isEmpty())
    {
        // TODO: ideally this message box should pop up after the dialog is shown
        QMessageBox::information(this, tr("Configure server"), m_Message);
    }
}

void ServerConfigDialog::accept()
{
    serverConfig().haveHeartbeat(m_pCheckBoxHeartbeat->isChecked());
    serverConfig().setHeartbeat(m_pSpinBoxHeartbeat->value());

    serverConfig().setRelativeMouseMoves(m_pCheckBoxRelativeMouseMoves->isChecked());
    serverConfig().setScreenSaverSync(m_pCheckBoxScreenSaverSync->isChecked());
    serverConfig().setWin32KeepForeground(m_pCheckBoxWin32KeepForeground->isChecked());

    serverConfig().haveSwitchDelay(m_pCheckBoxSwitchDelay->isChecked());
    serverConfig().setSwitchDelay(m_pSpinBoxSwitchDelay->value());

    serverConfig().haveSwitchDoubleTap(m_pCheckBoxSwitchDoubleTap->isChecked());
    serverConfig().setSwitchDoubleTap(m_pSpinBoxSwitchDoubleTap->value());

    serverConfig().setSwitchCorner(BaseConfig::SwitchCorner::TopLeft,
                                   m_pCheckBoxCornerTopLeft->isChecked());
    serverConfig().setSwitchCorner(BaseConfig::SwitchCorner::TopRight,
                                   m_pCheckBoxCornerTopRight->isChecked());
    serverConfig().setSwitchCorner(BaseConfig::SwitchCorner::BottomLeft,
                                   m_pCheckBoxCornerBottomLeft->isChecked());
    serverConfig().setSwitchCorner(BaseConfig::SwitchCorner::BottomRight,
                                   m_pCheckBoxCornerBottomRight->isChecked());
    serverConfig().setSwitchCornerSize(m_pSpinBoxSwitchCornerSize->value());
    serverConfig().setIgnoreAutoConfigClient(m_pCheckBoxIgnoreAutoConfigClient->isChecked());
    serverConfig().setEnableDragAndDrop(m_pCheckBoxEnableDragAndDrop->isChecked());
    serverConfig().setClipboardSharing(m_pCheckBoxEnableClipboard->isChecked());
    syncLegacyGrid();

    // now that the dialog has been accepted, copy the new server config to the original one,
    // which is a reference to the one in MainWindow.
    setOrigServerConfig(serverConfig());
    m_OrigServerConfig.saveSettings();

    QDialog::accept();
}

void ServerConfigDialog::onAddScreen()
{
    m_layoutEditor->addScreen();
}

void ServerConfigDialog::onEditScreen()
{
    m_layoutEditor->editSelectedScreen();
}

void ServerConfigDialog::onRemoveScreen()
{
    m_layoutEditor->removeSelectedScreen();
}

void ServerConfigDialog::onAutoLayout()
{
    m_layoutEditor->autoLayout();
}

void ServerConfigDialog::syncLegacyGrid()
{
    std::vector<Screen> ordered = serverConfig().screens();
    std::sort(ordered.begin(), ordered.end(),
              [](const Screen& a, const Screen& b) {
                  if (a.isNull() != b.isNull()) {
                      return !a.isNull();
                  }
                  if (a.position().y() != b.position().y()) {
                      return a.position().y() < b.position().y();
                  }
                  return a.position().x() < b.position().x();
              });
    serverConfig().setScreens(ordered);
}

void ServerConfigDialog::on_m_pButtonNewHotkey_clicked()
{
    Hotkey hotkey;
    HotkeyDialog dlg(this, hotkey);
    if (dlg.exec() == QDialog::Accepted)
    {
        serverConfig().hotkeys().push_back(hotkey);
        m_pListHotkeys->addItem(hotkey.text());
    }
}

void ServerConfigDialog::on_m_pButtonEditHotkey_clicked()
{
    int idx = m_pListHotkeys->currentRow();
    Q_ASSERT(idx >= 0 && idx < serverConfig().hotkeys().size());
    Hotkey& hotkey = serverConfig().hotkeys()[idx];
    HotkeyDialog dlg(this, hotkey);
    if (dlg.exec() == QDialog::Accepted)
        m_pListHotkeys->currentItem()->setText(hotkey.text());
}

void ServerConfigDialog::on_m_pButtonRemoveHotkey_clicked()
{
    int idx = m_pListHotkeys->currentRow();
    Q_ASSERT(idx >= 0 && idx < serverConfig().hotkeys().size());
    serverConfig().hotkeys().erase(serverConfig().hotkeys().begin() + idx);
    m_pListActions->clear();
    delete m_pListHotkeys->item(idx);
}

void ServerConfigDialog::on_m_pListHotkeys_itemSelectionChanged()
{
    bool itemsSelected = !m_pListHotkeys->selectedItems().isEmpty();
    m_pButtonEditHotkey->setEnabled(itemsSelected);
    m_pButtonRemoveHotkey->setEnabled(itemsSelected);
    m_pButtonNewAction->setEnabled(itemsSelected);

    if (itemsSelected && serverConfig().hotkeys().size() > 0)
    {
        m_pListActions->clear();

        int idx = m_pListHotkeys->row(m_pListHotkeys->selectedItems()[0]);

        // There's a bug somewhere around here: We get idx == 1 right after we deleted the next to last item, so idx can
        // only possibly be 0. GDB shows we got called indirectly from the delete line in
        // on_m_pButtonRemoveHotkey_clicked() above, but the delete is of course necessary and seems correct.
        // The while() is a generalized workaround for all that and shouldn't be required.
        while (idx >= 0 && idx >= serverConfig().hotkeys().size())
            idx--;

        Q_ASSERT(idx >= 0 && idx < serverConfig().hotkeys().size());

        const Hotkey& hotkey = serverConfig().hotkeys()[idx];
        for (const Action& action : hotkey.actions()) {
            m_pListActions->addItem(action.text());
        }
    }
}

void ServerConfigDialog::on_m_pButtonNewAction_clicked()
{
    int idx = m_pListHotkeys->currentRow();
    Q_ASSERT(idx >= 0 && idx < serverConfig().hotkeys().size());
    Hotkey& hotkey = serverConfig().hotkeys()[idx];

    Action action;
    ActionDialog dlg(this, serverConfig(), hotkey, action);
    if (dlg.exec() == QDialog::Accepted)
    {
        hotkey.appendAction(action);
        m_pListActions->addItem(action.text());
    }
}

void ServerConfigDialog::on_m_pButtonEditAction_clicked()
{
    int idxHotkey = m_pListHotkeys->currentRow();
    Q_ASSERT(idxHotkey >= 0 && idxHotkey < serverConfig().hotkeys().size());
    Hotkey& hotkey = serverConfig().hotkeys()[idxHotkey];

    int idxAction = m_pListActions->currentRow();
    Q_ASSERT(idxAction >= 0 && idxAction < hotkey.actions().size());
    Action action = hotkey.actions()[idxAction];

    ActionDialog dlg(this, serverConfig(), hotkey, action);
    if (dlg.exec() == QDialog::Accepted) {
        hotkey.setAction(idxAction, action);
        m_pListActions->currentItem()->setText(action.text());
    }
}

void ServerConfigDialog::on_m_pButtonRemoveAction_clicked()
{
    int idxHotkey = m_pListHotkeys->currentRow();
    Q_ASSERT(idxHotkey >= 0 && idxHotkey < serverConfig().hotkeys().size());
    Hotkey& hotkey = serverConfig().hotkeys()[idxHotkey];

    int idxAction = m_pListActions->currentRow();
    Q_ASSERT(idxAction >= 0 && idxAction < hotkey.actions().size());

    hotkey.removeAction(idxAction);
    delete m_pListActions->currentItem();
}

void ServerConfigDialog::on_m_pListActions_itemSelectionChanged()
{
    m_pButtonEditAction->setEnabled(!m_pListActions->selectedItems().isEmpty());
    m_pButtonRemoveAction->setEnabled(!m_pListActions->selectedItems().isEmpty());
}
