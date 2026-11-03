# Remote control: use the keyboard and mouse at your desk on the server

Etherwaver normally shares the **server's** keyboard and mouse with the
clients. Remote control adds the other direction: a client forwards its
**own** local keyboard and mouse to the server (or to another computer the
server manages) over the existing Etherwaver connection.

Typical use: the server sits in a rack, and a wireless receiver plugged into
it keeps losing its keyboard and mouse because the cabinet blocks the radio.
Plug the receiver (or wired devices) into the laptop or PC you sit at,
run the Etherwaver client there, and press **Control with local mouse and
keyboard**.

```
receiver at your desk ──USB──▶ Etherwaver client ══TLS══▶ Etherwaver server (rack)
                               captures input            injects it through its
                               (X11 grab / evdev)        normal input path
```

The feature forwards input events. It is not USB passthrough: the receiver
stays owned by the client computer.

## How it works

* Protocol 1.8 adds four messages on the existing (TLS, fingerprint-verified)
  connection: request control (`RCRQ`), release control (`RCRL`), control
  state (`RCST`) and forwarded input (`RINP`).
* The server's `RemoteControlArbiter` grants control to **one** client at a
  time. Every grant has an *epoch*; input with another epoch is dropped, so
  events from an old session are never replayed after a reconnect.
* Accepted input goes through the server's existing input path. When the
  server's own screen is active it is synthesized there (XTest, SendInput or
  CGEvent, depending on the server platform). When another client is active it
  is relayed exactly like the server's physical input, so screen edges,
  hotkeys, keyboard broadcasting and input filters keep working.
* The server never switches the focus to the controlling client. Without
  that rule the client's input would loop back to it. The client also ignores
  input from the server while it forwards.
* Keys and buttons are tracked on both sides. Everything that is still
  pressed is released when control ends for any reason (release, release
  shortcut, disconnect, timeout, takeover, a change of the controlled host).
* The client queue is bounded (1024 events). Mouse motion and wheel events
  are merged when the link is slow. Presses and releases are never dropped;
  if they cannot be queued, remote control stops and everything is released.
* Typed characters are never logged. Only state changes, counters and errors
  are.

## Setting it up

### 1. Server: allow the client to control

Remote control is **off by default**: a connected client can be controlled,
but it may not control the server. Allow it per screen:

* GUI: *Configure Server → double-click the client → Fixes →* **Allow this
  computer to control the server with its own mouse and keyboard**.
* Configuration file:

```
section: screens
    rack:
    laptop:
        remoteControl = true      # may control the server
    viewer:                       # may only be controlled ("view only")
end

section: options
    # remoteControl = true        # optional default for all screens
    remoteControlTakeover = true  # another permitted client may take over (default true)
    remoteControlTimeout = 3000   # ms without client messages before held keys are released
end
```

The server still accepts only clients that pass the existing TLS fingerprint
check.

### 2. Client: start remote control

GUI (client mode): the **Control the server with this computer's mouse and
keyboard** panel shows the current state, which is one of *Local control*,
*Remote control* or *View only*. It has these controls:

* **Control with local mouse and keyboard**: requests control. The first time,
  a dialog shows the emergency release shortcut.
* **Release mouse and keyboard**: returns the input to this computer.
* **Take over control**: shown when another computer is in control.
* **Send Ctrl+Alt+Del**: sends the combination as an explicit action.
* **Mode**: *Full capture*, or *Session window*.
* **Computer**: leave it empty to control the server, or enter another
  screen name.
* **Emergency release shortcut**: default `Ctrl+Alt+Shift+Q`.

While control is active the panel says, for example: *Mouse and keyboard now
control: rack. Press Ctrl+Alt+Shift+Q to return to this computer.*

Command line / headless clients use the local control API (127.0.0.1 only):

```bash
waverc --name laptop --remote-control-release-hotkey "Ctrl+Alt+Shift+Q" rack.example:24800
```

```bash
TOKEN=$(python3 -c 'import json,os;print(json.load(open(os.path.expanduser("~/.local/share/etherwaver/remote-control-api.json")))["token"])')
```

```bash
curl -s -X POST -H "X-Etherwaver-Token: $TOKEN" "http://127.0.0.1:24803/remote-control/start?mode=capture"
```

```bash
curl -s -X POST -H "X-Etherwaver-Token: $TOKEN" http://127.0.0.1:24803/remote-control/stop
```

`GET /remote-control` returns the status as JSON. Other parameters are
`target=<screen>`, `takeover=1` and `POST /remote-control/ctrl-alt-del`. The
token file is created with owner-only permissions (Unix; on Windows it lives
in the user's profile directory) and removed when the client exits.

Client options:

| option | default | meaning |
|---|---|---|
| `--remote-control-release-hotkey <keys>` | `Ctrl+Alt+Shift+Q` | local emergency shortcut |
| `--remote-control-api-port <port>` | `24803` | local control API, `0` disables it |
| `--remote-control-backend auto\|x11\|evdev\|none` | `auto` | capture backend (Linux) |
| `--remote-control-xkb-layout <layout>` / `--remote-control-xkb-variant <v>` | system layout | keyboard layout for the evdev backend, e.g. `gb`, `pl` |

The server's status is also available read-only at
`http://<server>:24802/get/remote-control`. That endpoint is the existing
HTTP API and has no authentication.

## Modes

| | Full capture | Session window |
|---|---|---|
| what is captured | every key and the mouse (global grab) | only while the Etherwaver window has the focus |
| pointer | hidden, relative motion | absolute: the position in the window is mapped to the remote screen, with letterbox bars and HiDPI scale taken into account |
| system shortcuts (Alt+Tab, Super…) | forwarded on X11/evdev | stay with the local window manager |
| losing focus | not applicable | forwarding pauses and everything held is released |

## Getting back to local control

Any of these returns the keyboard and mouse to the client computer and
releases everything pressed on the remote host:

* the **Release mouse and keyboard** button or `POST /remote-control/stop`;
* the **emergency shortcut** (`Ctrl+Alt+Shift+Q`, configurable). It is
  detected locally by the capture backend, is never sent to the server, and
  works even when the connection is down;
* loss of the connection (the client stops immediately; the server releases
  on its own);
* sleep, detected from the gap between the boot and monotonic clocks, or the
  system's suspend notification;
* the system breaking the grab, the session window being closed, or a capture
  device being unplugged;
* the server revoking control (takeover, timeout, permission removed);
* closing or crashing the client.

The default shortcut was chosen because it does not clash with any known
reserved combination: Windows (Ctrl+Alt+Del, Ctrl+Shift+Esc), macOS
(Cmd+Opt+Esc and Cmd+Shift+Q use Command, not Control), GNOME, KDE (Ctrl+Alt+Esc
is xkill), or the Linux VT switch (Ctrl+Alt+Fn). Q is in the same position on
UK and Polish Programmer keyboards. The shortcut is matched on the unshifted
symbol of the key.

### What the server does if the client dies

| situation | server reaction |
|---|---|
| client closes or crashes (TCP closed) | releases all held keys and buttons at once (measured: 0.11 s) |
| network silent (cable pulled, client frozen) | after `remoteControlTimeout` (3 s) it releases held input; after keep-alive death (~9 s) or 15 s it revokes control |
| reconnect | no automatic capture; old epochs are rejected |

## Keyboard details

* The protocol carries both the symbol (`KeyID`) and the physical key
  (`KeyButton`). Presses and releases are paired by the physical key, so a
  release always matches its press even when modifiers change in between.
* Text is reproduced by symbol, using the server's own keyboard layout. AltGr
  characters (Polish Programmer `ą ć ę ł ń ó ś ź ż`) and UK symbols
  (`" £ # \ @`) were verified end to end. If the two computers use different
  layouts, a symbol that does not exist on the server's layout cannot be
  typed. This is the same rule the rest of Etherwaver follows.
* Autorepeat: repeats are forwarded and the server injects them through its
  normal repeat path. A key held for 1.5 s gave 24 repeats through Etherwaver
  and 23 typed locally, so repeats are not doubled.
* Ctrl+Alt+Del is a separate button. It is generated as the real secure
  attention sequence only when the server can do that (a Windows server
  screen). Elsewhere it is sent as an ordinary key combination.

## Platform support

| client platform | status |
|---|---|
| Linux, X11 | **Supported and verified**: full capture and session window (Xvfb end-to-end tests, see below) |
| Linux, Wayland | Full capture through the **evdev** backend (exclusive `EVIOCGRAB`). This needs read access to `/dev/input/event*`, usually by joining the `input` group. The key translation for `gb` and `pl` is unit tested. Grabbing real devices still has to be checked on hardware. Under XWayland the X11 backend offers the session window only, because a global X11 grab does not see native Wayland apps |
| Linux console / headless | evdev backend, same permissions |
| Windows client | **not implemented yet**: the panel shows "Input capture is not available" (planned backend: low-level keyboard and mouse hooks, `LLKHF_INJECTED` filtering) |
| macOS client | **not implemented yet** (planned: CGEventTap, which needs the *Input Monitoring* and *Accessibility* permissions) |
| browser | Etherwaver has no browser client |

| server platform | injection |
|---|---|
| Linux X11 | XTest. Verified |
| Linux Wayland | XTest reaches XWayland apps only. Native Wayland apps on the server do not receive the input |
| Windows / macOS servers | use the existing `fake*` injection of their screen. The new code is platform independent C++, but it was built and tested on Linux only: **not verified** on Windows or macOS |

Permissions on the client:

* **evdev**: membership of the `input` group (`sudo usermod -aG input $USER`,
  then log in again). While capturing, the devices are grabbed exclusively, so
  neither the console nor the compositor sees the input.
* **X11**: none. In full-capture mode a screen locker cannot grab the keyboard
  until you release it (this is how every X11 grab behaves, VirtualBox's
  included).

## Step by step: receiver at the user → Etherwaver client → server in the rack

1. On the rack server, start the Etherwaver server as usual. In *Configure
   Server*, add the client screen (e.g. `laptop`), open its settings and tick
   **Allow this computer to control the server with its own mouse and
   keyboard**. Apply.
2. Plug the USB receiver (or wired keyboard and mouse) into the laptop. On
   Linux Wayland or a console, add the user to the `input` group.
3. Start the Etherwaver client on the laptop in client mode, with the server's
   address. Accept the server's fingerprint if asked.
4. In the client window, in **Control the server with this computer's mouse
   and keyboard**, pick *Full capture* and press **Control with local mouse
   and keyboard**. Read the shortcut in the dialog and confirm.
5. The panel shows *Remote control*: the receiver's keyboard and mouse now
   drive the server. Mouse movement past the server's screen edges reaches
   other Etherwaver hosts as usual, but never the laptop itself.
6. Press **Ctrl+Alt+Shift+Q**, or **Release mouse and keyboard**, to use the
   laptop again.

## Verification

Automated (`bin/unittests`, `remote/` tests):

* `RemoteInputTests`: held-state tracking and release order, coalescing in
  the bounded queue, overflow behaviour, hotkey parsing and matching,
  letterbox and HiDPI mapping, wire validation.
* `RemoteControlArbiterTests`: grant and deny, view-only, one controller,
  takeover, stale epochs, lease release and revoke, disconnect release.
* `ServerRemoteControlTests`: the server input path, permissions (screen and
  global options), stale epochs, conflict and takeover, controller disconnect,
  timeout, host switch (releasing on the old host, routing to the new one),
  the loop guard (never switching onto the controller), and loss of the
  controlled host.
* `RemoteControlControllerTests`: explicit start only, forwarding order,
  release shortcut, release button, connection loss without replay, suspend,
  capture loss, revoke, view-only, missing permission, failed grab, late grant,
  focus handling in window mode, unmatched releases, host change, overflow,
  Ctrl+Alt+Del, and status without key content.
* `RemoteControlProtocolTests`: wire round trip of the 1.8 messages.
* `EvdevKeyTranslatorTests`: UK and Polish Programmer layouts, modifiers,
  function keys and keypad.

End to end: Xvfb "rack" server and "laptop", "desk" and "viewer" clients,
over plain TCP and over mutual TLS 1.3, with input generated by XTest on the
laptop display:

* typing, Ctrl/Alt/Shift/Super combinations, F-keys, navigation keys and the
  keypad;
* AltGr characters (pl) and UK symbols (gb);
* clicks on buttons 1–3 and 8–9, vertical and horizontal scroll, dragging,
  relative motion;
* window mode coordinates with letterbox bars;
* the release button and the shortcut, with no keys or buttons left pressed
  locally or remotely;
* the network frozen with Ctrl and a mouse button held (released after about
  3 s);
* a client crash with Shift and a button held (released in 0.11 s);
* closing the client;
* reconnecting without replay;
* switching between the server and another host;
* two clients in conflict, then a takeover;
* refusal for a view-only client;
* the edge loop guard;
* classic KVM (server to client) unchanged.

### Hardware tests still to do (cannot be done in CI)

* A real wireless receiver on a Linux Wayland laptop with the evdev backend:
  exclusive grab, hot-unplug of the receiver while controlling, and suspend
  with the lid closed.
* Windows and macOS **servers**: injection of AltGr, keypad and
  Ctrl+Alt+Del through SendInput / CGEvent.
* HiDPI client displays (scale 150–200 %) in session-window mode.
* Long-distance Wi-Fi: behaviour of merged motion under packet loss.
