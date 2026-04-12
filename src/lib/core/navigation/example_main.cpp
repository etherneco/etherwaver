/*
 * example_main.cpp
 *
 * Full wiring example:
 *
 *   AppScreen   – logical, stable IDs ("alice:screen0", "bob:screen1", …)
 *   PhysicalScreen – Qt QScreen wrapper (no stable names/indices)
 *   ScreenContext  – logical + physical + platform
 *   NavigationManager – drives all screen-to-screen transitions
 *   INavigationBridge  – abstract cross-system switch interface
 *   ServerNavigationBridge – concrete bridge that calls Server::switchToScreenName
 *
 * Layout modelled:
 *
 *   [alice:screen0] --right--> [alice:screen1]
 *        |                          |
 *       down                       down
 *        |                          |
 *   [bob:screen0]  --right--> [bob:screen1]
 *
 *   alice and bob are two separate host systems (cross-host transitions
 *   go through ServerNavigationBridge → Server::switchToScreenName).
 *
 * Build (standalone verification, no actual Server):
 *   g++ -std=c++17 example_main.cpp \
 *       $(pkg-config --cflags --libs Qt6Gui Qt6Core) \
 *       -I<repo>/src/lib
 */

#include "core/navigation/AppScreen.hpp"
#include "core/navigation/INavigationBridge.hpp"
#include "core/navigation/NavigationManager.hpp"
#include "core/navigation/PhysicalScreen.hpp"
#include "core/navigation/ScreenContext.hpp"

#include <QCoreApplication>
#include <QDebug>
#include <QGuiApplication>
#include <QObject>
#include <iostream>

// ---------------------------------------------------------------------------
// Stub bridge (used when running without a live Server instance).
// Accepts every reachable cross-host transition and logs it.
// ---------------------------------------------------------------------------
class StubNavigationBridge : public etherwaver::navigation::INavigationBridge {
public:
    bool executeTransition(const etherwaver::navigation::TransitionRequest& req) override
    {
        std::cout
            << "[BRIDGE] executeTransition"
            << " from=" << req.fromScreenId
            << " to="   << req.toScreenId
            << " dir="  << static_cast<int>(req.direction)
            << (req.isCrossHost ? " CROSS-HOST" : " same-host")
            << "\n";
        // In production this calls Server::switchToScreenName(req.toScreenId).
        return true;
    }

    bool isScreenReachable(const std::string& screenId) const override
    {
        // Stub: all screens are reachable.
        (void)screenId;
        return true;
    }
};

// ---------------------------------------------------------------------------
// Build the four-screen, two-host layout
// ---------------------------------------------------------------------------
static void buildLayout(etherwaver::navigation::NavigationManager& mgr)
{
    using namespace etherwaver::navigation;

    // alice:screen0  (primary, top-left)
    AppScreen a0("alice:screen0", "alice", "screen0");
    a0.setMeta("label", "Alice Left");
    ScreenLinks la0;
    la0.right  = "alice:screen1";
    la0.bottom = "bob:screen0";      // cross-host ↓
    a0.setLinks(la0);

    // alice:screen1  (top-right)
    AppScreen a1("alice:screen1", "alice", "screen1");
    a1.setMeta("label", "Alice Right");
    ScreenLinks la1;
    la1.left   = "alice:screen0";
    la1.bottom = "bob:screen1";      // cross-host ↓
    a1.setLinks(la1);

    // bob:screen0  (bottom-left)
    AppScreen b0("bob:screen0", "bob", "screen0");
    b0.setMeta("label", "Bob Left");
    ScreenLinks lb0;
    lb0.right = "bob:screen1";
    lb0.top   = "alice:screen0";     // cross-host ↑
    b0.setLinks(lb0);

    // bob:screen1  (bottom-right)
    AppScreen b1("bob:screen1", "bob", "screen1");
    b1.setMeta("label", "Bob Right");
    ScreenLinks lb1;
    lb1.left = "bob:screen0";
    lb1.top  = "alice:screen1";      // cross-host ↑
    b1.setLinks(lb1);

    mgr.addScreen(ScreenContext(a0));
    mgr.addScreen(ScreenContext(a1));
    mgr.addScreen(ScreenContext(b0));
    mgr.addScreen(ScreenContext(b1));
}

// ---------------------------------------------------------------------------
// Observer
// ---------------------------------------------------------------------------
class NavigationObserver : public QObject {
    Q_OBJECT
public:
    explicit NavigationObserver(etherwaver::navigation::NavigationManager& mgr,
                                QObject* parent = nullptr)
        : QObject(parent)
    {
        connect(&mgr, &etherwaver::navigation::NavigationManager::screenChanged,
                this, &NavigationObserver::onScreenChanged);
        connect(&mgr, &etherwaver::navigation::NavigationManager::hostChanged,
                this, &NavigationObserver::onHostChanged);
        connect(&mgr, &etherwaver::navigation::NavigationManager::physicalScreenDetached,
                this, &NavigationObserver::onPhysicalDetached);
    }

private slots:
    void onScreenChanged(const QString& newId, const QString& prevId)
    {
        qDebug() << "[NAV] screen:" << prevId << "->" << newId;
    }

    void onHostChanged(const QString& newHost, const QString& prevHost)
    {
        qDebug() << "[NAV] HOST:"   << prevHost << "->" << newHost
                 << "(cross-system transition)";
    }

    void onPhysicalDetached(const QString& logicalId)
    {
        qDebug() << "[PHYS] monitor detached from" << logicalId
                 << "– navigation state unchanged";
    }
};

// ---------------------------------------------------------------------------
int main(int argc, char* argv[])
{
    QGuiApplication app(argc, argv);
    using namespace etherwaver::navigation;

    // 1. Create manager and wire bridge.
    NavigationManager mgr;
    StubNavigationBridge bridge;   // replace with ServerNavigationBridge(&server)
    mgr.setBridge(&bridge);

    NavigationObserver observer(mgr);

    // 2. Register screens.
    buildLayout(mgr);
    mgr.setInitialScreen("alice:screen0");

    std::cout << "\n=== Same-host transition (alice → alice) ===\n";
    // alice:screen0 →[right]→ alice:screen1
    // Bridge called: same-host, no actual client switch needed beyond cursor.
    mgr.navigateInDirection(EDirection::Right);

    std::cout << "\n=== Cross-host transition (alice → bob) ===\n";
    // alice:screen1 →[down]→ bob:screen1
    // Bridge called: isCrossHost=true → ServerNavigationBridge::executeTransition
    // → Server::switchToScreenName("bob:screen1")
    // → getClientForLayoutScreen → switchScreen (leave alice, enter bob)
    mgr.navigateInDirection(EDirection::Bottom);

    std::cout << "\n=== Cross-host back (bob → alice) ===\n";
    // bob:screen1 →[top]→ alice:screen1
    mgr.navigateInDirection(EDirection::Top);

    std::cout << "\n=== Direct jump to bob:screen0 ===\n";
    // Explicit cross-host jump, no direction.
    mgr.navigateTo("bob:screen0");

    std::cout << "\n=== Navigate back ===\n";
    mgr.navigateBack();

    std::cout << "\n=== State ===\n";
    qDebug() << "Current :" << QString::fromStdString(mgr.currentScreenId());
    qDebug() << "Previous:" << QString::fromStdString(mgr.previousScreenId());

    if (const ScreenContext* ctx = mgr.currentContext()) {
        qDebug() << "Platform:" << ctx->platformName();
    }

    std::cout << "\n=== Registered screens ===\n";
    for (const std::string& id : mgr.allScreenIds()) {
        const ScreenContext* c = mgr.getContext(id);
        std::cout << "  " << id
                  << "  [" << c->appScreen().meta("label") << "]"
                  << (c->hasPhysicalScreen() ? "  (physical bound)" : "")
                  << "\n";
    }

    return 0;
}

#include "example_main.moc"
