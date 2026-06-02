#!/usr/bin/env sh
set -eu

UUID="etherwaver-unsafe-mode@local"
EXT_DIR="${HOME}/.local/share/gnome-shell/extensions/${UUID}"

if [ "$(id -u)" -eq 0 ]; then
    echo "Run this as the desktop user, not root." >&2
    echo "Example: ./dist/udev/enable-gnome-unsafe-mode.sh" >&2
    exit 1
fi

SHELL_VERSION="$(gnome-shell --version 2>/dev/null | sed -n 's/.* \([0-9][0-9]*\)\..*/\1/p' | head -1)"
if [ -z "$SHELL_VERSION" ]; then
    SHELL_VERSION="49"
fi

mkdir -p "$EXT_DIR"

cat > "${EXT_DIR}/metadata.json" <<EOF
{
  "uuid": "${UUID}",
  "name": "EtherWaver Unsafe Mode",
  "description": "Enables GNOME Shell unsafe mode so EtherWaver can query global.get_pointer() through org.gnome.Shell.Eval.",
  "shell-version": ["${SHELL_VERSION}"]
}
EOF

cat > "${EXT_DIR}/extension.js" <<'EOF'
import {Extension} from 'resource:///org/gnome/shell/extensions/extension.js';

export default class EtherWaverUnsafeModeExtension extends Extension {
    enable() {
        this._previousUnsafeMode = global.context.unsafe_mode;
        global.context.unsafe_mode = true;
    }

    disable() {
        global.context.unsafe_mode = this._previousUnsafeMode ?? false;
    }
}
EOF

gnome-extensions enable "$UUID" 2>/dev/null || true

python3 - "$UUID" <<'PY'
import ast
import subprocess
import sys

uuid = sys.argv[1]

def read_list(key):
    try:
        raw = subprocess.check_output(
            ["gsettings", "get", "org.gnome.shell", key],
            text=True,
        ).strip()
        if raw.startswith("@as "):
            raw = raw[4:]
        value = ast.literal_eval(raw)
        return value if isinstance(value, list) else []
    except Exception:
        return []

def write_list(key, values):
    rendered = "[" + ", ".join(repr(value) for value in values) + "]"
    subprocess.call(["gsettings", "set", "org.gnome.shell", key, rendered])

enabled = read_list("enabled-extensions")
if uuid not in enabled:
    enabled.append(uuid)
write_list("enabled-extensions", enabled)

disabled = [value for value in read_list("disabled-extensions") if value != uuid]
write_list("disabled-extensions", disabled)
PY

echo "Installed GNOME Shell extension: ${UUID}"
echo "Path: ${EXT_DIR}"
echo
echo "Log out and back in, then test with:"
echo "  gdbus call --session --dest org.gnome.Shell --object-path /org/gnome/Shell --method org.gnome.Shell.Eval 'global.context.unsafe_mode'"
echo
echo "Expected:"
echo "  (true, 'true')"
