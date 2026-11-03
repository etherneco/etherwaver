#!/usr/bin/env python3
"""End-to-end test: browser -> HTTP API -> adapter -> real wavers -> UI.

Starts, in an isolated HOME:
  * two Xvfb displays (server desktop, client desktop),
  * waver-web (through its command line, as installed) managing wavers,
  * the Playwright scenario script e2e.js, which also starts waverc and
    edits the shared settings with the real desktop panel classes.

usage: run_e2e.py --bin-dir <dir with wavers/waverc> [--harness <desktop_config>] [--shots <dir>]
"""

import argparse
import os
import shutil
import signal
import socket
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
WEB = os.path.abspath(os.path.join(HERE, "..", ".."))
PASSWORD = "e2e-password-123"


def free_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    p = s.getsockname()[1]
    s.close()
    return p


def free_display(start):
    for n in range(start, start + 60):
        if not os.path.exists("/tmp/.X11-unix/X%d" % n) and not os.path.exists("/tmp/.X%d-lock" % n):
            return ":%d" % n
    raise SystemExit("no free X display")


def wait_port(port, timeout=10):
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            socket.create_connection(("127.0.0.1", port), 0.2).close()
            return True
        except OSError:
            time.sleep(0.1)
    return False


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--bin-dir", required=True)
    ap.add_argument("--harness", default=os.environ.get("DESKTOP_CONFIG_HARNESS", ""))
    ap.add_argument("--shots", default="")
    ap.add_argument("--keep", action="store_true")
    a = ap.parse_args()

    home = tempfile.mkdtemp(prefix="ew-e2e-")
    procs = []
    env = dict(os.environ, HOME=home, XDG_CONFIG_HOME=os.path.join(home, ".config"),
               XDG_DATA_HOME=os.path.join(home, ".local", "share"), PYTHONPATH=WEB)
    env.pop("WAVER_WEB_CONFIG", None)
    try:
        d1 = free_display(160)
        procs.append(subprocess.Popen(["Xvfb", d1, "-screen", "0", "1920x1080x24"], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL))
        time.sleep(0.4)
        d2 = free_display(int(d1[1:]) + 1)
        procs.append(subprocess.Popen(["Xvfb", d2, "-screen", "0", "1280x800x24"], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL))
        time.sleep(0.4)

        web_port, control_port, kvm_port = free_port(), free_port(), free_port()
        cfg_dir = os.path.join(home, ".config", "etherwaver")
        os.makedirs(cfg_dir)
        with open(os.path.join(cfg_dir, "web.ini"), "w") as fh:
            fh.write("[web]\nlisten = 127.0.0.1:%d\n\n[etherwaver]\ncontrol_address = 127.0.0.1:%d\nrequest_timeout = 3\n\n"
                     "[service]\nmanage = true\nbin_dir = %s\ndisplay = %s\n" % (web_port, control_port, a.bin_dir, d1))
        for user, role in (("admin", "admin"), ("viewer", "viewer")):
            subprocess.run([sys.executable, "-m", "waver_web", "user", "add", user, "--role", role, "--password-stdin"],
                           input=PASSWORD + "\n", text=True, env=env, check=True, cwd=WEB)
        subprocess.run([sys.executable, "-m", "waver_web.build", "--check-js"], env=env, check=True, cwd=WEB)
        log = open(os.path.join(home, "waver-web.log"), "w")
        web = subprocess.Popen([sys.executable, "-m", "waver_web", "serve"], env=env, cwd=WEB, stdout=log, stderr=subprocess.STDOUT)
        procs.append(web)
        if not wait_port(web_port):
            raise SystemExit("waver-web did not start")

        e2e_env = dict(env, BASE_URL="http://127.0.0.1:%d/" % web_port, PASSWORD=PASSWORD,
                       CLIENT_DISPLAY=d2, BIN_DIR=a.bin_dir, KVM_PORT=str(kvm_port),
                       SETTINGS_FILE=os.path.join(home, ".config", "Debauchee", "EtherWaver.conf"),
                       HARNESS=a.harness, SHOTS=a.shots)
        r = subprocess.run(["node", os.path.join(HERE, "e2e.js")], env=e2e_env)
        print("waver-web log:", os.path.join(home, "waver-web.log"))

        # 14. graceful shutdown: SIGTERM stops the managed process too
        web.send_signal(signal.SIGTERM)
        try:
            web.wait(15)
            print("PASS 14b graceful shutdown (exit code %s)" % web.returncode)
        except subprocess.TimeoutExpired:
            print("FAIL 14b graceful shutdown timed out")
            r = subprocess.CompletedProcess([], 1)
        leftovers = subprocess.run(["pgrep", "-f", "wavers.*%s" % control_port], capture_output=True, text=True).stdout.strip()
        if leftovers:
            print("FAIL managed wavers still running after shutdown:", leftovers)
            r = subprocess.CompletedProcess([], 1)
        return r.returncode
    finally:
        for p in reversed(procs):
            if p.poll() is None:
                p.terminate()
                try:
                    p.wait(5)
                except subprocess.TimeoutExpired:
                    p.kill()
        if not a.keep:
            shutil.rmtree(home, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
