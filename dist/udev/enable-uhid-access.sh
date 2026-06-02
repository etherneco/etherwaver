#!/usr/bin/env sh
set -eu

RULE_PATH="/etc/udev/rules.d/70-etherwaver-uhid.rules"
MODULES_PATH="/etc/modules-load.d/etherwaver-uhid.conf"
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
# Allow an active local EtherWaver session to create UHID keyboard/mouse devices.
SUBSYSTEM=="misc", KERNEL=="uhid", MODE="0660", GROUP="input", TAG+="uaccess"
EOF

cat > "$MODULES_PATH" <<'EOF'
uhid
EOF

modprobe uhid

if command -v udevadm >/dev/null 2>&1; then
    udevadm control --reload-rules
    udevadm trigger --subsystem-match=misc || true
fi

chgrp input /dev/uhid 2>/dev/null || true
chmod g+rw /dev/uhid 2>/dev/null || true

if command -v setfacl >/dev/null 2>&1; then
    setfacl -m "u:${TARGET_USER}:rw" /dev/uhid 2>/dev/null || true
fi

echo "Installed $RULE_PATH"
echo "Installed $MODULES_PATH"
echo "Loaded uhid and granted /dev/uhid read/write access to '$TARGET_USER' via group 'input'."
echo "Log out and back in if the current shell does not see the new group yet."
