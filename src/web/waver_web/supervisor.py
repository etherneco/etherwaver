"""Optional supervision of the Etherwaver core process by the web backend.

The desktop panel starts ``wavers``/``waverc`` as its child process.  On a
headless machine (or when the desktop panel is closed) the web backend can
take that role.  The command line is built exactly like
``MainWindow::serverArgs`` / ``MainWindow::clientArgs`` from the shared
configuration.

The supervisor never fights another owner: if the Etherwaver server answers
on the control endpoint with a PID that is not our child, it is reported as
"external" and start/stop are refused (the owner - desktop panel or system
service - must be used).
"""

from __future__ import annotations

import asyncio
import logging
import os
import shutil
import signal
import sys
import time
from collections import deque
from typing import Any, Callable, Deque, Dict, List, Optional

from . import model

log = logging.getLogger("waver_web.supervisor")

SHUTDOWN_CH = b"S"  # ShutdownCh.h


class SupervisorError(Exception):
    def __init__(self, code: str, message: str):
        super().__init__(message)
        self.code = code
        self.message = message


def find_executable(name: str, bin_dir: Optional[str]) -> Optional[str]:
    if bin_dir:
        candidate = os.path.join(bin_dir, name)
        if os.path.isfile(candidate) and os.access(candidate, os.X_OK):
            return candidate
        return None
    return shutil.which(name)


class Supervisor:
    def __init__(self, enabled: bool, bin_dir: Optional[str], work_dir: str, control_address: str,
                 on_output: Callable[[str, str], None], on_state: Callable[[Dict[str, Any]], None],
                 display: Optional[str] = None, extra_args: Optional[List[str]] = None) -> None:
        self.enabled = enabled
        self.bin_dir = bin_dir
        self.work_dir = work_dir
        self.control_address = control_address
        self.display = display
        self.extra_args = extra_args or []
        self._on_output = on_output
        self._on_state = on_state
        self.proc: Optional[asyncio.subprocess.Process] = None
        self.mode: Optional[str] = None
        self.expected_running = False
        self.started_at: Optional[float] = None
        self.exit_code: Optional[int] = None
        self.restarts = 0
        self.client_connected = False
        self.command: List[str] = []
        self._readers: List[asyncio.Task] = []
        self._watch: Optional[asyncio.Task] = None
        self._lock = asyncio.Lock()
        self.recent_output: Deque[str] = deque(maxlen=200)
        self._snapshot_provider: Optional[Callable[[], Dict[str, Any]]] = None

    @property
    def pid(self) -> Optional[int]:
        return self.proc.pid if self.proc and self.proc.returncode is None else None

    @property
    def running(self) -> bool:
        return self.pid is not None

    def info(self) -> Dict[str, Any]:
        return {
            "enabled": self.enabled,
            "running": self.running,
            "pid": self.pid,
            "mode": self.mode,
            "expectedRunning": self.expected_running,
            "startedAt": self.started_at,
            "lastExitCode": self.exit_code,
            "restarts": self.restarts,
            "clientConnected": self.client_connected if self.mode == "client" else None,
            "command": [c for c in self.command],
        }

    def build_command(self, snapshot: Dict[str, Any]) -> List[str]:
        """Port of MainWindow::startBarrier + serverArgs/clientArgs."""
        g = snapshot["general"]
        mode = g["mode"]
        exe_name = "wavers" if mode == "server" else "waverc"
        exe = find_executable(exe_name, self.bin_dir)
        if not exe:
            raise SupervisorError("not_installed", "the %s executable was not found" % exe_name)
        args = [exe, "-f", "--no-tray", "--debug", g["logLevel"], "--name", g["screenName"] or os.uname().nodename]
        if not g["cryptoEnabled"]:
            args.append("--disable-crypto")
        if g["logToFile"]:
            args += ["--log", g["logFilename"]]
        if mode == "server":
            if sys.platform != "linux" and snapshot["layout"]["options"]["enableDragAndDrop"]:
                args.append("--enable-drag-drop")
            if not g["requireClientCertificate"]:
                args.append("--disable-client-cert-checking")
            config_path = self._prepare_server_config(snapshot)
            address = ("[%s]" % g["interface"] if g["interface"] else "") + ":" + str(g["port"])
            args += ["-c", config_path, "--address", address]
            if self.control_address:
                args += ["--control-address", self.control_address]
        else:
            if sys.platform.startswith("linux") and g["uhidEnabled"]:
                args.append("--uhid=enable")
                if g["uhidName"].strip():
                    args += ["--uhid-name", g["uhidName"].strip()]
            host = g["serverHostname"].strip()
            if not host:
                raise SupervisorError("invalid_config",
                                      "set the server host name (automatic discovery is only available in the desktop panel)")
            args.append("[%s]:%d" % (host, g["port"]))
        if self.display:
            args += ["--display", self.display]
        return args + self.extra_args

    def _prepare_server_config(self, snapshot: Dict[str, Any]) -> str:
        g = snapshot["general"]
        if g["configSource"] == "external":
            path = g["externalConfigFile"]
            if not path or not os.path.isfile(path):
                raise SupervisorError("invalid_config", "the external configuration file does not exist")
            return path
        os.makedirs(self.work_dir, exist_ok=True)
        path = os.path.join(self.work_dir, "etherwaver.conf")
        with open(path, "w", encoding="utf-8") as fh:
            fh.write(model.server_config_text(snapshot["layout"]))
        with open(os.path.join(self.work_dir, "etherwaver-layout.json"), "w", encoding="utf-8") as fh:
            fh.write(model.layout_json_text(snapshot["layout"]))
        return path

    def config_path(self) -> str:
        return os.path.join(self.work_dir, "etherwaver.conf")

    async def start(self, snapshot_provider: Callable[[], Dict[str, Any]]) -> Dict[str, Any]:
        if not self.enabled:
            raise SupervisorError("not_managed", "process management by the web panel is disabled")
        async with self._lock:
            if self.running:
                raise SupervisorError("already_running", "the Etherwaver process is already running")
            self._snapshot_provider = snapshot_provider
            self.expected_running = True
            await self._spawn()
            return self.info()

    async def _spawn(self) -> None:
        assert self._snapshot_provider is not None
        snapshot = self._snapshot_provider()
        cmd = self.build_command(snapshot)
        self.command = cmd
        self.mode = snapshot["general"]["mode"]
        self.client_connected = False
        env = dict(os.environ)
        self.proc = await asyncio.create_subprocess_exec(
            *cmd, stdin=asyncio.subprocess.PIPE, stdout=asyncio.subprocess.PIPE,
            stderr=asyncio.subprocess.STDOUT, env=env, start_new_session=True)
        self.started_at = time.time()
        self.exit_code = None
        self._readers = [asyncio.ensure_future(self._pump(self.proc))]
        self._watch = asyncio.ensure_future(self._wait_exit(self.proc))
        self._on_state(self.info())

    async def _pump(self, proc: asyncio.subprocess.Process) -> None:
        assert proc.stdout is not None
        while True:
            line = await proc.stdout.readline()
            if not line:
                return
            text = line.decode("utf-8", errors="replace").rstrip("\r\n")
            if not text:
                continue
            self.recent_output.append(text)
            # same heuristics as MainWindow::checkConnected
            if ("connected to server" in text or "server status: active" in text) and self.mode == "client":
                if not self.client_connected:
                    self.client_connected = True
                    self._on_state(self.info())
            if "disconnected from server" in text and self.client_connected:
                self.client_connected = False
                self._on_state(self.info())
            self._on_output("process", text)

    async def _wait_exit(self, proc: asyncio.subprocess.Process) -> None:
        code = await proc.wait()
        self.exit_code = code
        for r in self._readers:
            try:
                await asyncio.wait_for(r, 1.0)
            except (asyncio.TimeoutError, asyncio.CancelledError):
                pass
        self._on_output("supervisor", "process exited with code %s" % code)
        self._on_state(self.info())
        # MainWindow::barrierFinished: restart when the stop was not requested
        if self.expected_running and self.proc is proc:
            self.restarts += 1
            await asyncio.sleep(1.0)
            if self.expected_running and self.proc is proc:
                try:
                    async with self._lock:
                        if self.expected_running and not self.running:
                            self._on_output("supervisor", "process not running, auto restarting")
                            await self._spawn()
                except Exception as e:
                    self._on_output("supervisor", "restart failed: %s" % e)

    async def stop(self, timeout: float = 5.0) -> Dict[str, Any]:
        if not self.enabled:
            raise SupervisorError("not_managed", "process management by the web panel is disabled")
        async with self._lock:
            self.expected_running = False
            proc = self.proc
            if proc is None or proc.returncode is not None:
                return self.info()
            try:
                if proc.stdin is not None:
                    proc.stdin.write(SHUTDOWN_CH)
                    await proc.stdin.drain()
            except (ConnectionError, OSError):
                pass
            try:
                await asyncio.wait_for(proc.wait(), timeout)
            except asyncio.TimeoutError:
                try:
                    os.killpg(proc.pid, signal.SIGTERM)
                except ProcessLookupError:
                    pass
                try:
                    await asyncio.wait_for(proc.wait(), 3.0)
                except asyncio.TimeoutError:
                    os.killpg(proc.pid, signal.SIGKILL)
                    await proc.wait()
            self._on_state(self.info())
            return self.info()

    async def shutdown(self) -> None:
        if self.running:
            try:
                await self.stop()
            except Exception:  # pragma: no cover
                log.exception("failed to stop the managed process")
