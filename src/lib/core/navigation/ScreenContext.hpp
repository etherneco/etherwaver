/*
 * Etherwaver -- screen context (logical + physical + platform)
 *
 * ScreenContext binds together:
 *   - AppScreen    : the stable logical identity and navigation metadata
 *   - PhysicalScreen* : the Qt hardware abstraction (may be nullptr)
 *   - Platform     : detected OS enum, used only as passive context
 *
 * The physical binding is intentionally optional.  On Wayland there is no
 * reliable mechanism to map a logical screen to a specific physical monitor,
 * and the compositor may not export monitor geometry at all.  Navigation logic
 * must never block on a physical screen being present.
 *
 * Platform is detected once at runtime and stored here for informational
 * use (e.g. logging, telemetry).  No code path should branch on Platform
 * to alter navigation behaviour – that would re-introduce the coupling this
 * abstraction is designed to remove.
 */

#pragma once

#include "core/navigation/AppScreen.hpp"
#include "core/navigation/PhysicalScreen.hpp"

#include <QSysInfo>

namespace etherwaver {
namespace navigation {

// ---------------------------------------------------------------------------
// Platform enum
// ---------------------------------------------------------------------------
enum class Platform {
    Windows,
    Linux,
    macOS,
    Unknown
};

inline Platform detectPlatform()
{
#if defined(Q_OS_WIN)
    return Platform::Windows;
#elif defined(Q_OS_MACOS)
    return Platform::macOS;
#elif defined(Q_OS_LINUX)
    return Platform::Linux;
#else
    return Platform::Unknown;
#endif
}

inline const char* platformName(Platform p)
{
    switch (p) {
    case Platform::Windows: return "Windows";
    case Platform::macOS:   return "macOS";
    case Platform::Linux:   return "Linux";
    default:                return "Unknown";
    }
}

// ---------------------------------------------------------------------------
// ScreenContext
// ---------------------------------------------------------------------------
class ScreenContext {
public:
    ScreenContext() = default;

    // Construct with a logical screen.  Physical binding and platform are
    // set separately so that contexts can be created before Qt is fully
    // initialised (e.g. during config loading).
    explicit ScreenContext(const AppScreen& appScreen)
        : m_appScreen(appScreen)
        , m_platform(detectPlatform())
    {}

    ScreenContext(const AppScreen& appScreen,
                  PhysicalScreen* physical,
                  Platform platform = detectPlatform())
        : m_appScreen(appScreen)
        , m_physical(physical)
        , m_platform(platform)
    {}

    // --- Logical screen ---------------------------------------------------
    const AppScreen& appScreen() const        { return m_appScreen; }
    AppScreen&       appScreen()              { return m_appScreen; }
    const std::string& screenId() const       { return m_appScreen.id(); }

    // --- Physical binding -------------------------------------------------
    // May return nullptr.  Never assert on physical presence in logic code.
    const PhysicalScreen* physical() const    { return m_physical; }
    PhysicalScreen*       physical()          { return m_physical; }
    bool hasPhysicalScreen() const            { return m_physical && m_physical->isValid(); }

    void bindPhysical(PhysicalScreen* p)      { m_physical = p; }
    void unbindPhysical()                     { m_physical = nullptr; }

    // --- Platform (informational only) ------------------------------------
    Platform    platform() const              { return m_platform; }
    const char* platformName() const          { return etherwaver::navigation::platformName(m_platform); }

    bool isValid() const { return m_appScreen.isValid(); }

private:
    AppScreen      m_appScreen;
    PhysicalScreen* m_physical = nullptr;
    Platform       m_platform  = Platform::Unknown;
};

} // namespace navigation
} // namespace etherwaver
