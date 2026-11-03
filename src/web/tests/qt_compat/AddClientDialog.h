// Stub used only by the configuration compatibility harness.
#pragma once
#include <QString>
class QWidget;
enum { kAddClientRight, kAddClientLeft, kAddClientUp, kAddClientDown, kAddClientOther, kAddClientIgnore };
class AddClientDialog {
public:
    AddClientDialog(const QString&, QWidget*) {}
    int exec() { return 0; }
    int addResult() { return kAddClientIgnore; }
    bool ignoreAutoConfigClient() { return false; }
};
