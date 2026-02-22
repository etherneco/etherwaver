# Etherwaver

**Next-generation secure input mesh built on top of Barrier.**

Etherwaver extends traditional keyboard & mouse sharing with a modern,
modular architecture designed for Linux UHID, automation workflows, and
future Wayland compatibility.

> Not just screen sharing --- programmable input orchestration.

------------------------------------------------------------------------

## Why Etherwaver?

Barrier was originally designed for X11 desktop environments. Modern
Linux systems (especially Wayland-based) require a different approach.

Etherwaver introduces:

-   Native Linux UHID virtual device backend
-   Modular input injection layer
-   Experimental Host Control API
-   Infrastructure-friendly architecture
-   Headless-ready operation

------------------------------------------------------------------------

## Architecture

Etherwaver introduces a pluggable input backend abstraction layer:

IInputBackend\
├── X11Backend\
├── UHIDBackend\
└── (planned) WaylandBackend

Design Goals:

-   Decouple input injection from networking layer
-   Support virtual HID devices (UHID)
-   Improve Wayland compatibility
-   Enable API-driven screen switching
-   Maintain cross-platform support

------------------------------------------------------------------------

## Features

✔ Secure keyboard & mouse sharing\
✔ Cross-platform (Linux / Windows / macOS)\
✔ Linux UHID virtual device backend\
✔ Modular input injection layer\
✔ Headless-ready architecture\
✔ Experimental local Control API

------------------------------------------------------------------------

## Control API (Experimental)

Etherwaver exposes a local HTTP API for automation and scripting.

Example:

GET /api/host

POST /api/switch { "screen": "workstation-1" }

Use cases:

-   Automated screen switching
-   Integration with scripts
-   DevOps workflows
-   Remote orchestration

Authentication and token-based access control (planned).

------------------------------------------------------------------------

## Linux UHID Backend

Instead of relying solely on XTest injection, Etherwaver can create
virtual HID devices using Linux UHID.

Advantages:

-   Works closer to kernel input layer
-   Better Wayland compatibility
-   More realistic device emulation
-   Cleaner permission model for advanced setups

------------------------------------------------------------------------

## Build Instructions

Requirements (Linux):

-   cmake
-   libx11-dev
-   libxrandr-dev
-   libxi-dev
-   libudev-dev
-   build-essential

Build:

``` bash
git clone https://github.com/etherneco/etherwaver.git
cd etherwaver
mkdir build
cd build
cmake ..
make
```

------------------------------------------------------------------------

## Roadmap

-   [ ] Stable UHID backend
-   [ ] Wayland-native backend
-   [ ] Token-based API authentication
-   [ ] Remote configuration mode
-   [ ] Plugin-based backend loading
-   [ ] Binary renaming to `waver`

------------------------------------------------------------------------

## Relationship to Barrier

Etherwaver is a fork of Barrier, reimagined for modern Linux input
systems and automation environments.

It preserves the core networking logic while introducing:

-   Backend modularization
-   UHID injection support
-   API layer
-   Future Wayland focus

------------------------------------------------------------------------

## Motivation

Etherwaver was created to address limitations of traditional
input-sharing software in modern Linux environments.

As Wayland adoption grows and infrastructure-heavy setups become common,
input sharing must evolve beyond X11-based injection.

Etherwaver is an attempt to build that next layer.

------------------------------------------------------------------------

Website: https://etherwaver.com\
Maintained by: Etherneco Ltd, London, UK
