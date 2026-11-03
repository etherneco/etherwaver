"""Configuration of the web panel service itself (``web.ini``)."""

from __future__ import annotations

import configparser
import os
from dataclasses import dataclass, field
from typing import List, Optional, Tuple

from .store import default_profile_dir, default_settings_path

DEFAULT_LISTEN = "127.0.0.1:24880"
DEFAULT_CONTROL = "127.0.0.1:24803"


def default_config_path() -> str:
    base = os.environ.get("XDG_CONFIG_HOME") or os.path.join(os.path.expanduser("~"), ".config")
    return os.path.join(base, "etherwaver", "web.ini")


def split_hostport(value: str, default_port: int) -> Tuple[str, int]:
    value = value.strip()
    if value.startswith("["):
        host, _, rest = value[1:].partition("]")
        port = int(rest[1:]) if rest.startswith(":") else default_port
        return host, port
    if value.count(":") == 1:
        host, port = value.split(":")
        return host or "127.0.0.1", int(port)
    return value or "127.0.0.1", default_port


@dataclass
class WebSettings:
    listen: str = DEFAULT_LISTEN
    base_path: str = ""
    trusted_proxies: List[str] = field(default_factory=list)
    cookie_secure: str = "auto"          # auto | always | never
    allowed_origins: List[str] = field(default_factory=list)
    session_idle_minutes: int = 30
    session_max_hours: int = 12
    control_address: str = DEFAULT_CONTROL
    control_token_file: str = ""
    request_timeout: float = 5.0
    settings_file: str = ""
    gui_state_file: str = ""
    profile_dir: str = ""
    data_dir: str = ""
    static_dir: str = ""
    manage_service: bool = False
    bin_dir: str = ""
    display: str = ""
    log_level: str = "INFO"
    source: Optional[str] = None

    def resolve(self) -> "WebSettings":
        self.profile_dir = os.path.expanduser(self.profile_dir or default_profile_dir())
        self.settings_file = os.path.expanduser(self.settings_file or default_settings_path())
        self.gui_state_file = os.path.expanduser(self.gui_state_file or os.path.join(self.profile_dir, "gui-state.ini"))
        self.control_token_file = os.path.expanduser(self.control_token_file or os.path.join(self.profile_dir, "control-token"))
        self.data_dir = os.path.expanduser(self.data_dir or os.path.join(self.profile_dir, "web"))
        if not self.static_dir:
            here = os.path.dirname(os.path.abspath(__file__))
            self.static_dir = os.path.join(here, "static")
        if self.base_path and not self.base_path.startswith("/"):
            self.base_path = "/" + self.base_path
        self.base_path = self.base_path.rstrip("/")
        if self.cookie_secure not in ("auto", "always", "never"):
            raise ValueError("cookie_secure must be auto, always or never")
        return self

    @property
    def listen_hostport(self) -> Tuple[str, int]:
        return split_hostport(self.listen, 24880)

    @property
    def control_hostport(self) -> Tuple[str, int]:
        return split_hostport(self.control_address, 24803)


def load_settings(path: Optional[str]) -> WebSettings:
    s = WebSettings()
    candidate = path or os.environ.get("WAVER_WEB_CONFIG") or default_config_path()
    parser = configparser.ConfigParser(interpolation=None)
    if os.path.exists(candidate):
        parser.read(candidate, encoding="utf-8")
        s.source = candidate
    elif path:
        raise FileNotFoundError(path)

    def get(section: str, key: str, fallback: str = "") -> str:
        env = os.environ.get("WAVER_WEB_%s_%s" % (section.upper(), key.upper()))
        if env is not None:
            return env
        return parser.get(section, key, fallback=fallback)

    def lst(value: str) -> List[str]:
        return [v.strip() for v in value.replace("\n", ",").split(",") if v.strip()]

    s.listen = get("web", "listen", s.listen)
    s.base_path = get("web", "base_path", "")
    s.trusted_proxies = lst(get("web", "trusted_proxies", ""))
    s.cookie_secure = get("web", "cookie_secure", "auto")
    s.allowed_origins = lst(get("web", "allowed_origins", ""))
    s.session_idle_minutes = int(get("web", "session_idle_minutes", "30"))
    s.session_max_hours = int(get("web", "session_max_hours", "12"))
    s.static_dir = get("web", "static_dir", "")
    s.data_dir = get("web", "data_dir", "")
    s.log_level = get("web", "log_level", "INFO")
    s.control_address = get("etherwaver", "control_address", DEFAULT_CONTROL)
    s.control_token_file = get("etherwaver", "control_token_file", "")
    s.request_timeout = float(get("etherwaver", "request_timeout", "5"))
    s.settings_file = get("etherwaver", "settings_file", "")
    s.gui_state_file = get("etherwaver", "gui_state_file", "")
    s.profile_dir = get("etherwaver", "profile_dir", "")
    s.manage_service = get("service", "manage", "false").strip().lower() in ("1", "true", "yes", "on")
    s.bin_dir = get("service", "bin_dir", "")
    s.display = get("service", "display", "")
    return s.resolve()
