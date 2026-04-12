/*
 * Etherwaver -- server-side navigation bridge
 *
 * ServerNavigationBridge implements INavigationBridge for the Server.
 *
 * When NavigationManager resolves a logical transition (same-host OR
 * cross-host), it calls executeTransition().  This class translates the
 * request into the correct Server call:
 *
 *  Same-host,  intra-screen : Server::switchToScreenName(toScreenId)
 *  Cross-host              : Server::switchToScreenName(toScreenId)
 *                            (Server resolves the BaseClientProxy internally
 *                             via getClientForLayoutScreen / resolveClientHostId)
 *
 * Design notes:
 *  - Server already owns the authoritative ScreenManager; we do NOT duplicate
 *    layout logic here.  We rely on Server::switchToScreenName which performs
 *    the full sequence:  getClientForLayoutScreen → getJumpCursorPos →
 *    switchScreen → leave/enter/notify.
 *  - For direction-based transitions the Config path is bypassed: the screen
 *    ID was already resolved by NavigationManager via AppScreen::ScreenLinks.
 *    This is intentional — ScreenLinks is the single source of truth for
 *    logical adjacency.
 *  - isScreenReachable answers by checking whether the host owning `screenId`
 *    currently has a connected BaseClientProxy.
 */

#pragma once

#include "core/navigation/INavigationBridge.hpp"
#include "server/Server.h"

#include <string>

namespace etherwaver {
namespace server {

class ServerNavigationBridge : public navigation::INavigationBridge {
public:
    // `server` must outlive this bridge.
    explicit ServerNavigationBridge(Server* server)
        : m_server(server)
    {}

    // -----------------------------------------------------------------------
    // INavigationBridge
    // -----------------------------------------------------------------------

    bool executeTransition(const navigation::TransitionRequest& req) override
    {
        if (!m_server) {
            return false;
        }

        // Server::switchToScreenName handles both same-host and cross-host
        // cases:
        //   1. Looks up the etherwaver::layout::Screen by toScreenId.
        //   2. Calls getClientForLayoutScreen to resolve the BaseClientProxy.
        //   3. Gets the saved jump-cursor position for the destination.
        //   4. Calls switchScreen which performs leave/enter/clipboard sync.
        //
        // If the destination host is not connected, switchToScreenName returns
        // false and no state change happens on the server.
        return m_server->switchToScreenName(req.toScreenId);
    }

    bool isScreenReachable(const std::string& screenId) const override
    {
        if (!m_server) {
            return false;
        }

        // A screen is reachable if switchToScreenName would find a live
        // client for it.  We query the connected client list directly to
        // avoid a spurious switch side-effect.
        std::vector<std::string> connectedClients;
        m_server->getClients(connectedClients);

        // The screenId follows the "hostId:screenName" convention.
        // Extract the hostId part and check whether it is connected.
        const std::string::size_type colon = screenId.find(':');
        const std::string hostPart =
            (colon != std::string::npos) ? screenId.substr(0, colon) : screenId;

        for (const std::string& name : connectedClients) {
            if (name == hostPart || name == screenId) {
                return true;
            }
        }
        return false;
    }

private:
    Server* m_server;
};

} // namespace server
} // namespace etherwaver
