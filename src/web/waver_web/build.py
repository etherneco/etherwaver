"""Builds the web panel frontend into ``waver_web/static``.

The frontend uses native ES modules and needs no third-party toolchain.  The
build validates the sources, copies them into a content-hashed directory
(so browsers can cache assets forever) and writes ``index.html``.

    python3 -m waver_web.build [--src DIR] [--out DIR] [--check-js]
"""

from __future__ import annotations

import argparse
import hashlib
import os
import re
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
DEFAULT_SRC = os.path.join(HERE, "..", "frontend", "src")
DEFAULT_OUT = os.path.join(HERE, "static")

IMPORT_RE = re.compile(r"""(?:import|export)\s[^'"]*?from\s+['"](\.[^'"]+)['"]|import\(\s*['"](\.[^'"]+)['"]\s*\)""")


def iter_files(src):
    for base, _dirs, files in os.walk(src):
        for name in sorted(files):
            yield os.path.join(base, name)


def check_imports(src):
    problems = []
    for path in iter_files(src):
        if not path.endswith(".js"):
            continue
        with open(path, "r", encoding="utf-8") as fh:
            text = fh.read()
        for m in IMPORT_RE.finditer(text):
            rel = m.group(1) or m.group(2)
            target = os.path.normpath(os.path.join(os.path.dirname(path), rel))
            if not os.path.isfile(target):
                problems.append("%s: missing import %s" % (os.path.relpath(path, src), rel))
    return problems


def check_js_syntax(src):
    node = shutil.which("node")
    if not node:
        print("node not found; skipping JavaScript syntax check")
        return []
    problems = []
    for path in iter_files(src):
        if path.endswith(".js"):
            with open(path, "rb") as fh:
                r = subprocess.run([node, "--input-type=module", "--check"], stdin=fh,
                                   capture_output=True, text=True)
            if r.returncode != 0:
                problems.append("%s: %s" % (os.path.relpath(path, src), r.stderr.strip()))
    return problems


def build(src, out, check_js=False):
    src = os.path.abspath(src)
    out = os.path.abspath(out)
    problems = check_imports(src)
    if check_js:
        problems += check_js_syntax(src)
    if problems:
        for p in problems:
            print("error:", p, file=sys.stderr)
        raise SystemExit(1)
    digest = hashlib.sha256()
    for path in iter_files(src):
        if os.path.basename(path) == "index.html":
            continue
        digest.update(os.path.relpath(path, src).encode())
        with open(path, "rb") as fh:
            digest.update(fh.read())
    version = digest.hexdigest()[:12]
    if os.path.isdir(out):
        shutil.rmtree(out)
    assets = os.path.join(out, "assets", version)
    for path in iter_files(src):
        rel = os.path.relpath(path, src)
        if rel == "index.html":
            continue
        dest = os.path.join(assets, rel)
        os.makedirs(os.path.dirname(dest), exist_ok=True)
        shutil.copy2(path, dest)
    with open(os.path.join(src, "index.html"), "r", encoding="utf-8") as fh:
        index = fh.read().replace("%ASSETS%", "assets/" + version)
    with open(os.path.join(out, "index.html"), "w", encoding="utf-8") as fh:
        fh.write(index)
    print("built frontend %s -> %s" % (version, out))
    return out


def main(argv=None):
    p = argparse.ArgumentParser()
    p.add_argument("--src", default=DEFAULT_SRC)
    p.add_argument("--out", default=DEFAULT_OUT)
    p.add_argument("--check-js", action="store_true", help="run node --check on every module")
    a = p.parse_args(argv)
    build(a.src, a.out, a.check_js)


if __name__ == "__main__":
    main()
