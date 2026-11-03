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

//! X11 capture of the local keyboard and mouse
/*!
Full capture: an active keyboard and pointer grab on the root window of
a private display connection, so no other X client receives the input;
the pointer is hidden and kept near the centre to measure relative
motion.  Window mode: a session window; input is forwarded only while it
has the focus and pointer positions inside it are mapped to the remote
screen.  Under Wayland (XWayland) a global X11 grab cannot capture
native Wayland applications, so only window mode is offered there; use
the evdev backend for full capture.
*/
typedef union _XEvent* XEventPtr;

class XWindowsLocalInputCapture : public ILocalInputCapture {
public:
    explicit XWindowsLocalInputCapture(const char* displayName);
    virtual ~XWindowsLocalInputCapture();

    virtual std::string name() const { return "x11"; }
    virtual bool        isAvailable(std::string& whyNot) const;
    virtual bool        supportsMode(Mode mode) const;
    virtual bool        start(Mode mode, const Config& config, Sink* sink, std::string& error);
    virtual void        stop();
    virtual void        updateTarget(const Config& config);
    virtual bool        isCapturing() const { return m_running; }

    //! true when running inside a Wayland session (XWayland)
    static bool         isWaylandSession();

private:
    struct Impl;
    void                run(Mode mode);
    bool                setup(Mode mode, Impl& s, std::string& error);
    void                loop(Mode mode, Impl& s);
    void                cleanup(Mode mode, Impl& s);
    void                handleEvent(Mode mode, Impl& s, XEventPtr event);
    void                redraw(Impl& s);
    void                reportStart(const std::string& error);
    void                wakeThread();

private:
    std::string         m_displayName;
    std::atomic<bool>   m_running;
    std::atomic<bool>   m_stopRequested;
    std::thread         m_thread;
    int                 m_wakePipe[2];
    mutable std::mutex  m_configMutex;
    Config              m_config;
    bool                m_configChanged;
    Sink*               m_sink;
    std::condition_variable m_startCv;
    bool                m_startDone;
    std::string         m_startError;
};
