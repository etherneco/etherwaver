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
#include "barrier/RemoteInput.h"

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

//! What the controller needs from the network side (implemented by Client)
class IRemoteControlTransport {
public:
    virtual ~IRemoteControlTransport() { }
    virtual bool        isConnectedToServer() const = 0;
    //! connected and the server speaks protocol 1.8
    virtual bool        isRemoteControlSupported() const = 0;
    virtual void        sendRemoteControlRequest(bool takeover, const std::string& target) = 0;
    virtual void        sendRemoteControlRelease(UInt32 epoch) = 0;
    virtual void        sendRemoteInput(UInt32 epoch, const barrier::remote::Event& event) = 0;
    virtual std::string serverName() const = 0;
};

//! Client side of remote control
/*!
Two explicit modes of the client:
 - local control (kLocal): the keyboard and mouse operate this computer,
   nothing is forwarded;
 - remote control (kRemote): input is captured and forwarded only to the
   granted session; the local system does not act on it.

Remote control is entered only through start() (a deliberate user
action: button, local API call).  Connecting or reconnecting never
starts it.  Every way out -- release button, release hotkey, lost
connection, revoke, sleep, capture loss -- ends in stop(), which gives
the input back to this computer and releases everything that was
pressed on the remote host.

All public methods except the Sink callbacks must be called from the
main (event queue) thread.  The Sink callbacks come from the capture
thread; they only queue work and call the wake-up function.
*/
class RemoteControlController : public ILocalInputCapture::Sink {
public:
    enum State {
        kLocal,      // input belongs to this computer
        kRequesting, // waiting for the server to grant control
        kRemote,     // input is forwarded to the remote host
        kSuspended   // granted, but forwarding paused (window lost focus)
    };

    struct Status {
        State           m_state = kLocal;
        ILocalInputCapture::Mode m_mode = ILocalInputCapture::kModeCapture;
        bool            m_connected = false;
        bool            m_serverSupports = false;
        bool            m_captureAvailable = false;
        bool            m_viewOnly = false;          // server refused: no permission
        std::string     m_captureBackend;
        std::string     m_captureUnavailableReason;
        std::string     m_target;                    // controlled host
        std::string     m_controller;                // who controls (may be another client)
        std::string     m_server;
        std::string     m_releaseHotkey;
        std::string     m_lastEvent;                 // machine readable code
        std::string     m_lastMessage;               // English text
        bool            m_windowModeSupported = false;
        bool            m_captureModeSupported = false;
        bool            m_ctrlAltDelNative = false;
        UInt32          m_epoch = 0;
        std::uint64_t   m_forwarded = 0;
        std::uint64_t   m_coalesced = 0;
        std::uint64_t   m_dropped = 0;
    };

    RemoteControlController(IRemoteControlTransport* transport,
                            const barrier::remote::Hotkey& releaseHotkey);
    ~RemoteControlController();

    //! Takes ownership.  May be null (no capture on this platform).
    void                setCapture(std::unique_ptr<ILocalInputCapture> capture);
    //! Called (from any thread) when processPending() should run.
    void                setWakeup(std::function<void()> wakeup) { m_wakeup = wakeup; }
    //! Called after every state change (main thread)
    void                setStatusListener(std::function<void(const Status&)> f)
                        { m_statusListener = f; }

    //! @name user commands
    //@{
    bool                start(ILocalInputCapture::Mode mode, const std::string& target,
                            bool takeover, std::string& error);
    //! Back to local control.  \p code says why ("user", "hotkey", ...)
    void                stop(const std::string& code, const std::string& message);
    //! Send ctrl+alt+del to the remote host as an explicit action
    bool                sendCtrlAltDel(std::string& error);
    //@}

    //! @name notifications from the client
    //@{
    void                onConnected();
    void                onDisconnected();
    void                onSuspend();
    void                onServerState(int state, int reason, UInt32 epoch,
                            SInt32 x, SInt32 y, SInt32 w, SInt32 h,
                            UInt8 capabilities, const std::string& controller,
                            const std::string& target);
    //@}

    //! Drain events queued by the capture thread (main thread)
    void                processPending();
    //! Periodic work: heartbeat (call about once per second)
    void                tick();

    //! true while incoming input from the server must be ignored
    bool                isForwarding() const { return m_state != kLocal; }
    State               state() const { return m_state; }
    UInt32              epoch() const { return m_epoch; }
    Status              status() const;
    static std::string  statusToJson(const Status& status);
    static const char*  stateName(State state);

    // ILocalInputCapture::Sink (capture thread)
    virtual void        onCaptureEvent(const barrier::remote::Event& event);
    virtual void        onCaptureReleaseHotkey();
    virtual void        onCaptureLost(const std::string& reason);
    virtual void        onCaptureFocus(bool active);

private:
    void                setState(State state);
    void                setMessage(const std::string& code, const std::string& message);
    void                notify();
    void                wake();
    void                forward(const barrier::remote::Event& event);
    void                releaseRemoteHeld();
    void                endLocally(const std::string& code, const std::string& message,
                            bool sendRelease);
    void                clearInbox();
    ILocalInputCapture::Config captureConfig() const;

private:
    IRemoteControlTransport* m_transport;
    std::unique_ptr<ILocalInputCapture> m_capture;
    barrier::remote::Hotkey m_releaseHotkey;
    std::function<void()> m_wakeup;
    std::function<void(const Status&)> m_statusListener;

    State               m_state;
    ILocalInputCapture::Mode m_mode;
    std::string         m_requestedTarget;
    UInt32              m_epoch;
    std::string         m_target;
    std::string         m_controller;
    SInt32              m_targetX, m_targetY, m_targetW, m_targetH;
    UInt8               m_capabilities;
    bool                m_viewOnly;
    std::string         m_lastEvent;
    std::string         m_lastMessage;

    // what the server was told is pressed
    barrier::remote::HeldInputState m_sentHeld;

    // capture thread -> main thread
    mutable std::mutex  m_inboxMutex;
    barrier::remote::InputQueue m_inbox;
    bool                m_inboxOverflow;
    bool                m_releaseHotkeyPressed;
    std::string         m_captureLost;
    std::vector<bool>   m_focusChanges;
    std::atomic<bool>   m_wakePending;

    std::uint64_t       m_forwarded;
};
