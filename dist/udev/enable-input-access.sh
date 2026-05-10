#!/usr/bin/env sh
set -eu

RULE_PATH="/etc/udev/rules.d/71-etherwaver-input.rules"
TARGET_USER="${1:-${SUDO_USER:-}}"

if [ "$(id -u)" -ne 0 ]; then
    echo "Run as root, for example: sudo $0 ${USER:-}" >&2
    exit 1
fi

if [ -z "$TARGET_USER" ] || [ "$TARGET_USER" = "root" ]; then
    echo "Usage: sudo $0 USERNAME" >&2
    echo "When run through sudo, USERNAME defaults to SUDO_USER." >&2
    exit 1
fi

if ! id "$TARGET_USER" >/dev/null 2>&1; then
    echo "User not found: $TARGET_USER" >&2
    exit 1
fi

if ! getent group input >/dev/null 2>&1; then
    groupadd --system input
fi

usermod -aG input "$TARGET_USER"

cat > "$RULE_PATH" <<'EOF'
# Allow EtherWaver's local cursor helper to read raw pointer events.
# This is intentionally broad: Wayland compositors do not expose global cursor
# position, so the helper needs read access to the kernel input event stream.
SUBSYSTEM=="input", KERNEL=="event*", MODE="0660", GROUP="input", TAG+="uaccess"
EOF

if command -v udevadm >/dev/null 2>&1; then
    udevadm control --reload-rules
    udevadm trigger --subsystem-match=input || true
fi

chgrp input /dev/input/event* 2>/dev/null || true
chmod g+r /dev/input/event* 2>/dev/null || true

if command -v setfacl >/dev/null 2>&1; then
    setfacl -m "u:${TARGET_USER}:r" /dev/input/event* 2>/dev/null || true
fi

echo "Installed $RULE_PATH"
echo "Granted /dev/input/event* read access to user '$TARGET_USER' via group 'input'."
echo "Log out and back in if the current shell does not see the new group yet."
