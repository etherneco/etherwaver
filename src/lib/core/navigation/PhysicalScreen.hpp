/*
 * Etherwaver -- physical screen abstraction (Qt wrapper)
 *
 * PhysicalScreen wraps a QScreen pointer and exposes only the properties that
 * are safe and meaningful across all platforms, including Wayland:
 *
 *   - geometry()          : bounding rect in the virtual desktop coordinate
 *                           space reported by Qt (may be (0,0,w,h) on Wayland
 *                           when the compositor does not export global coords)
 *   - availableGeometry() : work-area rect (excludes panels, taskbars, docks)
 *   - devicePixelRatio()  : HiDPI scaling factor
 *
 * What is intentionally NOT exposed:
 *   - QScreen::name()   – not stable; changes across reboots / Wayland sessions
 *   - QScreen::handle() – platform-specific, defeats portability
 *   - QScreen index     – changes when monitors are hot-plugged
 *   - Global origin     – Wayland compositors may report (0,0) for all screens
 *
 * Ownership: PhysicalScreen does NOT own the QScreen.  QScreen lifetime is
 * managed by QGuiApplication.  Users must listen for QGuiApplication::
 * screenRemoved to invalidate any PhysicalScreen that wraps the removed screen.
 */

#pragma once

#include <QRect>
#include <QScreen>
#include <QString>

namespace etherwaver {
namespace navigation {

class PhysicalScreen {
public:
    // Construct from a live QScreen.  `qscreen` must not be null.
    explicit PhysicalScreen(QScreen* qscreen)
        : m_screen(qscreen)
    {
        Q_ASSERT(qscreen != nullptr);
    }

    // Screen rectangle in Qt's virtual desktop space.
    // On Wayland this is typically relative (0,0) unless the compositor
    // provides the xdg-output protocol – treat it as sizing info only.
    QRect geometry() const
    {
        return m_screen ? m_screen->geometry() : QRect();
    }

    // Available area (excluding system panels / taskbars).
    QRect availableGeometry() const
    {
        return m_screen ? m_screen->availableGeometry() : QRect();
    }

    // HiDPI pixel ratio (e.g. 2.0 on a 4K display at 200 % scaling).
    qreal devicePixelRatio() const
    {
        return m_screen ? m_screen->devicePixelRatio() : 1.0;
    }

    // Size helpers derived from geometry().
    int width()  const { return geometry().width();  }
    int height() const { return geometry().height(); }

    // Returns true if the underlying QScreen pointer is still live.
    // Callers should invalidate PhysicalScreen instances upon
    // QGuiApplication::screenRemoved.
    bool isValid() const { return m_screen != nullptr; }

    // Nullify after the underlying QScreen has been removed.
    void invalidate() { m_screen = nullptr; }

    // Raw pointer for Qt API calls that require it (e.g. window placement).
    // Use sparingly – prefer the typed accessors above.
    QScreen* qscreen() const { return m_screen; }

private:
    QScreen* m_screen = nullptr;
};

} // namespace navigation
} // namespace etherwaver
