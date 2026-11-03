/*
 * Etherwaver -- remote control of a server from a client's local input.
 *
 * This package is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * found in the file LICENSE that should have accompanied this file.
 *
 * This package is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#pragma once

#include "barrier/ILocalInputCapture.h"

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

struct xkb_context;
struct xkb_keymap;
struct xkb_state;

//! Translates Linux evdev key codes to barrier key events (xkbcommon)
/*!
The keyboard layout must match the one the user types with (the kernel
only knows physical keys).  It is taken from the arguments, else from
XKB_DEFAULT_LAYOUT / XKB_DEFAULT_VARIANT, else /etc/default/keyboard,
else "us".
*/
class EvdevKeyTranslator {
public:
    struct Key {
        Key() : m_id(kKeyNone), m_baseId(kKeyNone), m_mask(0), m_button(0) { }
        KeyID           m_id;      // symbol with the active modifiers
        KeyID           m_baseId;  // unshifted symbol (hotkey matching)
        KeyModifierMask m_mask;    // modifiers active before this key
        KeyButton       m_button;  // physical key (evdev code + 8)
    };

    EvdevKeyTranslator();
    ~EvdevKeyTranslator();

    static bool         isSupported();

    bool                init(const std::string& layout, const std::string& variant,
                            std::string& error);
    //! \p value: 1 press, 0 release, 2 autorepeat.  Updates the state.
    Key                 translate(unsigned int evdevCode, int value);
    const std::string&  layout() const { return m_layout; }

    //! layout/variant from the environment or /etc/default/keyboard
    static void         systemLayout(std::string& layout, std::string& variant);

private:
    xkb_context*        m_context;
    xkb_keymap*         m_keymap;
    xkb_state*          m_state;
    std::string         m_layout;
};

//! Full capture of local input devices through Linux evdev
/*!
Opens the keyboards and mice in /dev/input and takes an exclusive grab
(EVIOCGRAB) so neither the console, X11 nor a Wayland compositor sees
the input.  Works under Wayland and without any display.  Needs read
access to /dev/input/event* (usually membership of the "input" group).
Etherwaver's own virtual UHID devices are skipped (no input loop).
*/
class LinuxEvdevLocalInputCapture : public ILocalInputCapture {
public:
    LinuxEvdevLocalInputCapture(const std::string& layout, const std::string& variant,
                                const std::string& ignoredDevicePrefix);
    virtual ~LinuxEvdevLocalInputCapture();

    virtual std::string name() const { return "evdev"; }
    virtual bool        isAvailable(std::string& whyNot) const;
    virtual bool        supportsMode(Mode mode) const { return mode == kModeCapture; }
    virtual bool        start(Mode mode, const Config& config, Sink* sink, std::string& error);
    virtual void        stop();
    virtual void        updateTarget(const Config& config);
    virtual bool        isCapturing() const { return m_running; }

    //! exposed for tests
    static bool         shouldIgnoreDevice(const std::string& name, const std::string& prefix);

private:
    struct Device {
        int             m_fd;
        std::string     m_path;
        std::string     m_name;
        bool            m_keyboard;
        bool            m_pointer;
        bool            m_hiResWheel;
        bool            m_hiResHWheel;
    };

    void                run();
    bool                openDevices(std::vector<Device>& devices, std::string& error) const;
    void                closeDevices(std::vector<Device>& devices) const;
    void                reportStart(const std::string& error);

private:
    std::string         m_layout;
    std::string         m_variant;
    std::string         m_ignoredPrefix;
    std::atomic<bool>   m_running;
    std::atomic<bool>   m_stopRequested;
    std::thread         m_thread;
    int                 m_wakePipe[2];
    std::mutex          m_mutex;
    std::condition_variable m_startCv;
    bool                m_startDone;
    std::string         m_startError;
    Config              m_config;
    Sink*               m_sink;
};
