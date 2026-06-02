/*
 * Etherwaver -- logical screen abstraction
 *
 * AppScreen represents a named, addressable screen slot in the application's
 * virtual layout.  It is fully independent of Qt, the OS, and any physical
 * monitor.  The same AppScreen definition works identically on Windows,
 * Linux/X11, and Linux/Wayland.
 *
 * Identity is carried by `id` (a stable, application-assigned string such as
 * a UUID or "hostname:screen0").  Never use a QScreen name, a monitor index,
 * or global pixel coordinates as an identity – all of those are unstable under
 * Wayland.
 */

#pragma once

#include <map>
#include <string>

namespace etherwaver {
namespace navigation {

// ---------------------------------------------------------------------------
// Directional neighbour links (screen-to-screen, not system-to-system)
// ---------------------------------------------------------------------------
struct ScreenLinks {
    std::string left;
    std::string right;
    std::string top;
    std::string bottom;
};

// ---------------------------------------------------------------------------
// AppScreen
// ---------------------------------------------------------------------------
class AppScreen {
public:
    AppScreen() = default;

    AppScreen(const std::string& id,
              const std::string& hostId,
              const std::string& name)
        : m_id(id)
        , m_hostId(hostId)
        , m_name(name)
    {}

    // Stable identifier – must be unique across the whole layout.
    // Assign once (e.g. "alice:screen0") and never derive it from OS state.
    const std::string& id()     const { return m_id;     }
    const std::string& hostId() const { return m_hostId; }
    const std::string& name()   const { return m_name;   }

    // Arbitrary key/value metadata (e.g. "role"="primary", "label"="Left").
    // Safe to extend without changing the navigation contract.
    void setMeta(const std::string& key, const std::string& value)
    {
        m_meta[key] = value;
    }

    std::string meta(const std::string& key) const
    {
        std::map<std::string, std::string>::const_iterator it = m_meta.find(key);
        return (it != m_meta.end()) ? it->second : std::string();
    }

    const std::map<std::string, std::string>& allMeta() const { return m_meta; }

    // Directional neighbours expressed as AppScreen ids, not host names.
    void         setLinks(const ScreenLinks& links) { m_links = links; }
    ScreenLinks& links()                            { return m_links;  }
    const ScreenLinks& links() const                { return m_links;  }

    bool isValid() const { return !m_id.empty() && !m_hostId.empty(); }

    bool operator==(const AppScreen& other) const { return m_id == other.m_id; }
    bool operator!=(const AppScreen& other) const { return m_id != other.m_id; }

private:
    std::string m_id;
    std::string m_hostId;
    std::string m_name;
    ScreenLinks m_links;
    std::map<std::string, std::string> m_meta;
};

} // namespace navigation
} // namespace etherwaver
