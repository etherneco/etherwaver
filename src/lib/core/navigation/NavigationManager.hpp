/*
 * Etherwaver -- screen navigation manager
 *
 * NavigationManager owns the set of ScreenContext objects and drives all
 * screen-to-screen transitions.  It replaces the old system/host-based
 * navigation with purely logical screen IDs.
 *
 * Key design contracts:
 *
 *  1. All transitions reference AppScreen IDs, never QScreen names, monitor
 *     indices, or host names.
 *
 *  2. Physical screen bookkeeping reacts to QGuiApplication::screenAdded /
 *     screenRemoved.  Physical changes never invalidate logical navigation
 *     state.
 *
 *  3. No global coordinate math is performed here.  Spatial adjacency is
 *     resolved through the ScreenLinks stored on each AppScreen.
 *
 *  4. Wayland restriction: QGuiApplication::screens() is consulted only to
 *     populate PhysicalScreen wrappers.  The order of that list must not
 *     affect which logical screen is considered "current".
 *
 *  5. Cross-system transitions (different hostId) are delegated to an optional
 *     INavigationBridge.  When no bridge is set, the transition is accepted
 *     and state is updated (suitable for standalone/UI-only mode).  When a
 *     bridge is set (server mode), the bridge executes the actual client
 *     switch; the state is updated only if the bridge returns true.
 */

#pragma once

#include "core/navigation/INavigationBridge.hpp"
#include "core/navigation/ScreenContext.hpp"

#include <QGuiApplication>
#include <QObject>
#include <QString>

#include <map>
#include <memory>
#include <string>
#include <vector>

namespace etherwaver {
namespace navigation {

// ---------------------------------------------------------------------------
// Direction enum (mirrors layout EDirection without the legacy header dep)
// ---------------------------------------------------------------------------
enum class EDirection { Left, Right, Top, Bottom };

// ---------------------------------------------------------------------------
// NavigationManager
// ---------------------------------------------------------------------------
class NavigationManager : public QObject {
    Q_OBJECT

public:
    explicit NavigationManager(QObject* parent = nullptr)
        : QObject(parent)
    {
        connect(qGuiApp, &QGuiApplication::screenAdded,
                this, &NavigationManager::onScreenAdded);
        connect(qGuiApp, &QGuiApplication::screenRemoved,
                this, &NavigationManager::onScreenRemoved);
    }

    // --- Bridge (optional, set once before first transition) ---------------

    // Set the server-side bridge.  Does NOT take ownership.
    // Call with nullptr to detach.
    void setBridge(INavigationBridge* bridge) { m_bridge = bridge; }
    INavigationBridge* bridge() const         { return m_bridge; }

    // --- Screen registration -----------------------------------------------

    void addScreen(const ScreenContext& ctx)
    {
        m_contexts[ctx.screenId()] = ctx;
    }

    void removeScreen(const std::string& screenId)
    {
        m_contexts.erase(screenId);
        if (m_currentId  == screenId) { m_currentId.clear();  }
        if (m_previousId == screenId) { m_previousId.clear(); }
    }

    const ScreenContext* getContext(const std::string& screenId) const
    {
        auto it = m_contexts.find(screenId);
        return (it != m_contexts.end()) ? &it->second : nullptr;
    }

    std::vector<std::string> allScreenIds() const
    {
        std::vector<std::string> ids;
        ids.reserve(m_contexts.size());
        for (const auto& kv : m_contexts) {
            ids.push_back(kv.first);
        }
        return ids;
    }

    // --- Initial state -----------------------------------------------------

    bool setInitialScreen(const std::string& screenId)
    {
        if (m_contexts.find(screenId) == m_contexts.end()) {
            return false;
        }
        m_currentId  = screenId;
        m_previousId.clear();
        return true;
    }

    // --- Navigation --------------------------------------------------------

    // Transition to an explicit logical screen ID.
    //
    // Cross-host flow:
    //   1. Build TransitionRequest describing the hop.
    //   2. If a bridge is registered, call bridge->executeTransition().
    //      - On false  → abort; no state change.
    //      - On true   → fall through to commit.
    //   3. Commit state (currentId, previousId) and emit screenChanged.
    //
    // Same-host flow: bridge is still consulted (same-host intra-machine
    // transitions, e.g. moving between two monitors on alice, also need the
    // server to call switchScreen to send the correct cursor position to the
    // sub-screen).
    bool navigateTo(const std::string& screenId,
                    TransitionDirection direction = TransitionDirection::None)
    {
        if (screenId.empty() || screenId == m_currentId) {
            return false;
        }
        const ScreenContext* dstCtx = getContext(screenId);
        if (!dstCtx) {
            return false;
        }

        // Skip unreachable screens if bridge can tell us.
        if (m_bridge && !m_bridge->isScreenReachable(screenId)) {
            return false;
        }

        // Build the request before we update state so fromScreenId is still
        // the *current* screen.
        if (m_bridge) {
            const ScreenContext* srcCtx = currentContext();
            TransitionRequest req;
            req.fromScreenId = m_currentId;
            req.toScreenId   = screenId;
            req.fromHostId   = srcCtx ? srcCtx->appScreen().hostId() : std::string();
            req.toHostId     = dstCtx->appScreen().hostId();
            req.direction    = direction;
            req.isCrossHost  = (req.fromHostId != req.toHostId);

            if (!m_bridge->executeTransition(req)) {
                return false;
            }
        }

        commitTransition(screenId);
        return true;
    }

    // Transition by following a directional link from the current screen.
    bool navigateInDirection(EDirection dir)
    {
        const ScreenContext* ctx = currentContext();
        if (!ctx) {
            return false;
        }
        const std::string target = resolveLink(ctx->appScreen().links(), dir);
        if (target.empty()) {
            return false;
        }
        return navigateTo(target, edirToTdir(dir));
    }

    // Navigate back to the previous screen (one level).
    bool navigateBack()
    {
        if (m_previousId.empty()) {
            return false;
        }
        return navigateTo(m_previousId);
    }

    // --- State queries -----------------------------------------------------

    const std::string& currentScreenId()  const { return m_currentId;  }
    const std::string& previousScreenId() const { return m_previousId; }

    const ScreenContext* currentContext() const { return getContext(m_currentId); }

    bool hasScreen(const std::string& screenId) const
    {
        return m_contexts.find(screenId) != m_contexts.end();
    }

signals:
    // Emitted after every successful transition.
    void screenChanged(const QString& newScreenId,
                       const QString& previousScreenId);

    // Emitted when a cross-host transition completes.
    // Useful for UI components that need to know which system is "active".
    void hostChanged(const QString& newHostId, const QString& previousHostId);

    // Informational: physical monitor attached/detached.
    // Navigation logic must NOT react to these.
    void physicalScreenAttached(const QString& logicalScreenId);
    void physicalScreenDetached(const QString& logicalScreenId);

private slots:
    void onScreenAdded(QScreen* screen)
    {
        for (auto& kv : m_contexts) {
            ScreenContext& ctx = kv.second;
            if (ctx.hasPhysicalScreen()) {
                continue;
            }
            m_physicalScreens.emplace_back(std::make_unique<PhysicalScreen>(screen));
            ctx.bindPhysical(m_physicalScreens.back().get());
            emit physicalScreenAttached(QString::fromStdString(ctx.screenId()));
            return;
        }
        m_physicalScreens.emplace_back(std::make_unique<PhysicalScreen>(screen));
    }

    void onScreenRemoved(QScreen* screen)
    {
        for (auto& ps : m_physicalScreens) {
            if (!ps || ps->qscreen() != screen) {
                continue;
            }
            for (auto& kv : m_contexts) {
                if (kv.second.physical() == ps.get()) {
                    kv.second.unbindPhysical();
                    emit physicalScreenDetached(
                        QString::fromStdString(kv.second.screenId()));
                    break;
                }
            }
            ps->invalidate();
            break;
        }
    }

private:
    // Commit new state and fire signals.  Call only after bridge approval.
    void commitTransition(const std::string& newId)
    {
        const std::string prevId = m_currentId;

        // Capture host IDs before updating state.
        std::string prevHost;
        std::string newHost;
        if (const ScreenContext* c = getContext(prevId)) {
            prevHost = c->appScreen().hostId();
        }
        if (const ScreenContext* c = getContext(newId)) {
            newHost = c->appScreen().hostId();
        }

        m_previousId = prevId;
        m_currentId  = newId;

        emit screenChanged(QString::fromStdString(m_currentId),
                           QString::fromStdString(m_previousId));

        if (newHost != prevHost) {
            emit hostChanged(QString::fromStdString(newHost),
                             QString::fromStdString(prevHost));
        }
    }

    static std::string resolveLink(const ScreenLinks& links, EDirection dir)
    {
        switch (dir) {
        case EDirection::Left:   return links.left;
        case EDirection::Right:  return links.right;
        case EDirection::Top:    return links.top;
        case EDirection::Bottom: return links.bottom;
        }
        return {};
    }

    static TransitionDirection edirToTdir(EDirection d)
    {
        switch (d) {
        case EDirection::Left:   return TransitionDirection::Left;
        case EDirection::Right:  return TransitionDirection::Right;
        case EDirection::Top:    return TransitionDirection::Top;
        case EDirection::Bottom: return TransitionDirection::Bottom;
        }
        return TransitionDirection::None;
    }

    INavigationBridge*                               m_bridge  = nullptr;
    std::map<std::string, ScreenContext>             m_contexts;
    std::vector<std::unique_ptr<PhysicalScreen>>     m_physicalScreens;
    std::string                                      m_currentId;
    std::string                                      m_previousId;
};

} // namespace navigation
} // namespace etherwaver
