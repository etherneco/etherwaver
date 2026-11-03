"""Command line entry point: ``python3 -m waver_web`` / ``waver-web``."""

from __future__ import annotations

import argparse
import asyncio
import getpass
import json
import logging
import os
import sys

from . import __version__
from .auth import ROLES, UserStore
from .settings import load_settings


def _users(settings) -> UserStore:
    os.makedirs(settings.data_dir, mode=0o700, exist_ok=True)
    return UserStore(os.path.join(settings.data_dir, "users.json"))


def _read_password(args) -> str:
    if args.password_stdin:
        return sys.stdin.readline().rstrip("\n")
    first = getpass.getpass("Password: ")
    second = getpass.getpass("Repeat password: ")
    if first != second:
        raise SystemExit("passwords do not match")
    return first


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(prog="waver-web", description="Etherwaver web panel")
    parser.add_argument("--config", help="path to web.ini (default: ~/.config/etherwaver/web.ini)")
    parser.add_argument("--version", action="version", version="waver-web " + __version__)
    sub = parser.add_subparsers(dest="command")

    serve = sub.add_parser("serve", help="run the web panel (default)")
    serve.add_argument("--listen", help="address:port to listen on (default 127.0.0.1:24880)")

    user = sub.add_parser("user", help="manage web panel accounts")
    usub = user.add_subparsers(dest="action", required=True)
    add = usub.add_parser("add")
    add.add_argument("username")
    add.add_argument("--role", choices=ROLES, default="viewer")
    add.add_argument("--password-stdin", action="store_true", help="read the password from stdin")
    pw = usub.add_parser("passwd")
    pw.add_argument("username")
    pw.add_argument("--password-stdin", action="store_true")
    rm = usub.add_parser("remove")
    rm.add_argument("username")
    role = usub.add_parser("role")
    role.add_argument("username")
    role.add_argument("role", choices=ROLES)
    usub.add_parser("list")

    sub.add_parser("check", help="print the effective configuration and check access to files")

    args = parser.parse_args(argv)
    settings = load_settings(args.config)
    logging.basicConfig(level=getattr(logging, settings.log_level.upper(), logging.INFO),
                        format="%(asctime)s %(levelname)s %(name)s: %(message)s")

    if args.command == "user":
        store = _users(settings)
        try:
            if args.action == "add":
                store.add(args.username, _read_password(args), args.role)
                print("user %s created with role %s" % (args.username, args.role))
            elif args.action == "passwd":
                store.update(args.username, password=_read_password(args))
                print("password changed (the user's web sessions end when the service restarts)")
            elif args.action == "remove":
                store.remove(args.username)
                print("user %s removed" % args.username)
            elif args.action == "role":
                store.update(args.username, role=args.role)
                print("user %s is now %s" % (args.username, args.role))
            else:
                for u in store.list():
                    print("%-24s %-9s %s" % (u["username"], u["role"], "disabled" if u["disabled"] else ""))
        except (ValueError, KeyError, LookupError, PermissionError) as e:
            print("error: %s" % (e.args[0] if e.args else e), file=sys.stderr)
            return 1
        return 0

    if args.command == "check":
        info = {k: v for k, v in vars(settings).items()}
        info["users"] = len(_users(settings).list())
        info["static_built"] = os.path.isfile(os.path.join(settings.static_dir, "index.html"))
        print(json.dumps(info, indent=2, default=str))
        return 0

    if getattr(args, "listen", None):
        settings.listen = args.listen
    if not _users(settings).list():
        logging.warning("no web panel accounts exist yet; create one with: waver-web user add <name> --role admin")
    from .app import serve as run
    try:
        asyncio.run(run(settings))
    except KeyboardInterrupt:
        pass
    return 0


if __name__ == "__main__":
    sys.exit(main())
