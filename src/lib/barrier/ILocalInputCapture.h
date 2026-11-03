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

#include "barrier/RemoteInput.h"

#include <string>

//! Captures this computer's own keyboard and mouse (remote control)
/*!
Implementations run their own thread and report through \c Sink from
that thread; the sink must be thread safe.  While capturing, the local
system must not act on the captured input (as far as the platform
allows) and the release hotkey must be detected locally -- it is never
reported as an input event.
*/
class ILocalInputCapture {
public:
    enum Mode {
        //! Global grab: every key and the mouse go to the remote host,
        //! motion is relative.  Ends only with the release action.
        kModeCapture,
        //! Session window: input is forwarded only while the Etherwaver
        //! session window has the focus; the pointer position inside the
        //! window is mapped to the remote screen (absolute motion).
        kModeWindow
    };

    struct Config {
        Config() : m_targetX(0), m_targetY(0), m_targetW(0), m_targetH(0) { }
        barrier::remote::Hotkey m_releaseHotkey;
        std::string     m_targetName;
        SInt32          m_targetX, m_targetY, m_targetW, m_targetH;
    };

    class Sink {
    public:
        virtual ~Sink() { }
        //! A captured input event (never the release hotkey)
        virtual void    onCaptureEvent(const barrier::remote::Event& event) = 0;
        //! The release hotkey was pressed
        virtual void    onCaptureReleaseHotkey() = 0;
        //! Capture ended or was interrupted by the system (grab lost,
        //! sleep detected, window closed, device removed...)
        virtual void    onCaptureLost(const std::string& reason) = 0;
        //! Window mode: focus gained (\p active true) or lost
        virtual void    onCaptureFocus(bool active) = 0;
    };

    virtual ~ILocalInputCapture() { }

    //! Backend name for diagnostics ("x11", "evdev", ...)
    virtual std::string name() const = 0;

    //! Can the backend capture on this system?  If not, \p whyNot says
    //! what is missing (e.g. a system permission).
    virtual bool        isAvailable(std::string& whyNot) const = 0;

    virtual bool        supportsMode(Mode mode) const = 0;

    //! Start capturing.  Returns false and sets \p error on failure.
    virtual bool        start(Mode mode, const Config& config, Sink* sink,
                            std::string& error) = 0;

    //! Stop capturing and give the input back to the local system
    //! (cursor position and visibility restored).  Must be safe to call
    //! when not capturing.  Blocks until the capture thread has ended.
    virtual void        stop() = 0;

    //! Controlled host changed (window mode updates its mapping)
    virtual void        updateTarget(const Config& config) = 0;

    virtual bool        isCapturing() const = 0;
};
