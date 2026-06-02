/*
 * Etherwaver -- navigation bridge interface
 *
 * INavigationBridge decouples NavigationManager (logical, Qt) from the
 * server-side switching machinery (Server, BaseClientProxy).
 *
 * When NavigationManager resolves a transition it delegates execution here.
 * The concrete implementation (ServerNavigationBridge) translates it into a
 * Server::switchScreen / switchToScreenName call.
 *
 * No Qt, no server, no platform headers — purposely dependency-free.
 */

#pragma once

#include <string>

namespace etherwaver {
namespace navigation {

// Direction enum duplicated here to keep this header free of layout/ deps.
// Must stay in sync with layout/EDirection and NavigationManager::EDirection.
enum class TransitionDirection { Left, Right, Top, Bottom, None };

struct TransitionRequest {
    std::string fromScreenId;       // AppScreen id of current screen
    std::string toScreenId;         // AppScreen id of destination
    std::string fromHostId;         // hostId of current screen
    std::string toHostId;           // hostId of destination (may differ)
    TransitionDirection direction;  // edge that was crossed (or None for jump)
    bool isCrossHost;               // toHostId != fromHostId
};

// ---------------------------------------------------------------------------
// INavigationBridge
// ---------------------------------------------------------------------------
class INavigationBridge {
public:
    virtual ~INavigationBridge() = default;

    // Called by NavigationManager before committing a transition.
    //
    // Implementor must:
    //   1. Locate the BaseClientProxy that owns toScreenId
    //   2. Call Server::switchScreen (or equivalent) with appropriate coords
    //
    // Returns true  → transition accepted; NavigationManager updates state
    // Returns false → transition rejected; NavigationManager does NOT update state
    virtual bool executeTransition(const TransitionRequest& req) = 0;

    // Returns true when the client owning `screenId` is currently connected.
    // NavigationManager uses this to skip disabled screens.
    virtual bool isScreenReachable(const std::string& screenId) const = 0;
};

} // namespace navigation
} // namespace etherwaver
