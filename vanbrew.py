#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Vanbrew - a tiny universal package manager (pip + Homebrew in one file).

  * One requirement: Python (2.7 or 3.x). No third-party libraries, ever.
  * Same commands on macOS / Linux / Windows, old machines and new.
  * Installs Vanta and anything else you write a small recipe ("formula") for.

A formula just says WHAT files a package has and HOW to run it. Drop a .json
formula into ~/.vanbrew/registry and `vanbrew install <name>` works.

Why Python and not Vanta?  A bootstrapper can't depend on the thing it installs
- Vanbrew has to run BEFORE Vanta exists, so it leans on the one runtime that's
already on basically every computer: Python.
"""
from __future__ import print_function

import json
import os
import shutil
import stat
import sys

try:                       # py3
    import urllib.request as _urlreq
except ImportError:        # py2
    import urllib2 as _urlreq  # type: ignore

VERSION = "0.1.1"

HOME = os.path.expanduser("~")
VB_HOME = os.environ.get("VANBREW_HOME") or os.path.join(HOME, ".vanbrew")
BIN = os.path.join(VB_HOME, "bin")
CELLAR = os.path.join(VB_HOME, "cellar")
REG_DIR = os.path.join(VB_HOME, "registry")
STATE = os.path.join(VB_HOME, "installed.json")
IS_WIN = os.name == "nt"

# Where this script currently lives (so `setup` can install Vanbrew itself).
try:
    SELF = os.path.abspath(__file__)
except NameError:          # run from stdin (curl ... | python3 -)
    SELF = None

# ---- where packages are hosted (overridable so anyone can fork) -------------
RAW = os.environ.get("VANBREW_RAW") or \
    "https://raw.githubusercontent.com/Juanshep1/vanbrew/main"
REGISTRY_URL = RAW + "/registry.json"

def _u(path):
    return {"kind": "url", "url": RAW + path}

# ---- built-in catalog -------------------------------------------------------
# A formula: name, version, summary, optional deps, files[], bin[].
#   files: {"source": {...}, "as": "<filename in cellar>"}
#   source kinds: local{path} | url{url[,sha256]} | inline{text}
#   bin:   {"name": "<cmd>", "kind": "python|vanta|exec|shell", "main": "<file>"}
BUILTIN = {
    "vanta": {
        "version": "4.5",
        "summary": "The Vanta plain-English programming language & interpreter",
        "files": [{"source": _u("/packages/vanta/vanta.py"), "as": "vanta.py"}],
        "bin": [{"name": "vanta", "kind": "python", "main": "vanta.py"}],
    },
    "vcode": {
        "version": "4.1",
        "summary": "Vanta Code (vcode) - a Claude Code-style terminal coding agent for Vanta. `vanbrew install vcode`, then `vcode` (it prompts for your API key). edit/search/glob, diffs, themes, skills, run_app.",
        "deps": ["vanta"],
        "files": [{"source": _u("/packages/vcode/vcode.py"), "as": "vcode.py"}],
        "bin": [{"name": "vcode", "kind": "python", "main": "vcode.py"},
                {"name": "vanta-code", "kind": "python", "main": "vcode.py"}],
    },
    "vnox": {
        "version": "1.0",
        "summary": "V-NOx 1 - the first operating system written in Vanta (desktop OS, port 8100)",
        "deps": ["vanta"],
        "files": [{"source": _u("/packages/vnox/vnox.va"), "as": "vnox.va"}],
        "bin": [{"name": "vnox", "kind": "vanta", "main": "vnox.va"}],
    },
    "vself": {
        "version": "1.6",
        "summary": "Vanta-in-Vanta - a Vanta interpreter written in Vanta, shipped as NATIVE C so it runs with ZERO Python (just cc to build). `vself prog.va` runs functions/recursion/lists/maps/file I/O, and even runs web servers (serve()) and the vc compiler itself.",
        "files": [{"source": _u("/packages/vself/vanta.va.c"), "as": "vanta.va.c"},
                  {"source": _u("/packages/vself/vanta.va"), "as": "vanta.va"}],
        "bin": [{"name": "vself", "kind": "cc", "main": "vanta.va.c"}],
    },
    "vc": {
        "version": "1.6",
        "summary": "vc - a self-hosting Vanta-to-C compiler (in Vanta), shipped as NATIVE C so the compiler itself runs with ZERO Python (just cc to build it, then `vc prog.va` -> a native binary). Compiles strings/lists/maps/serve()/HTTP/JSON/filesystem + an Ebb GC - even itself and the interpreter.",
        "files": [{"source": _u("/packages/vc/vc.va.c"), "as": "vc.va.c"},
                  {"source": _u("/packages/vc/vc.va"), "as": "vc.va"}],
        "bin": [{"name": "vc", "kind": "cc", "main": "vc.va.c"}],
    },
    "vfmt": {
        "version": "1.0",
        "summary": "vfmt - the Vanta auto-formatter (like gofmt): re-indents .va files to 4 spaces per block, preserving comments. Native binary, no Python. Usage: vfmt file.va [-w]",
        "files": [{"source": _u("/packages/vfmt/vfmt.va.c"), "as": "vfmt.va.c"},
                  {"source": _u("/packages/vfmt/vfmt.va"), "as": "vfmt.va"}],
        "bin": [{"name": "vfmt", "kind": "cc", "main": "vfmt.va.c"}],
    },
    "topdeck": {
        "version": "1.0",
        "summary": "Yu-Gi-Oh! Master Duel meta analyzer (a Vanta web app, port 8090)",
        "deps": ["vanta"],
        "files": [{"source": _u("/packages/topdeck/mdmeta.va"), "as": "mdmeta.va"}],
        "bin": [{"name": "topdeck", "kind": "vanta", "main": "mdmeta.va"}],
    },
    "vaeldric": {
        "version": "1.0",
        "summary": "Vaeldric conlang site + JSON API (a Vanta web app, port 8080)",
        "deps": ["vanta"],
        "files": [{"source": _u("/packages/vaeldric/vaeldric.va"), "as": "vaeldric.va"}],
        "bin": [{"name": "vaeldric", "kind": "vanta", "main": "vaeldric.va"}],
    },
    "hello": {
        "version": "1.0",
        "summary": "A tiny demo package (proves Vanbrew installs more than Vanta)",
        "files": [{"source": {"kind": "inline", "text":
                   "#!/usr/bin/env python3\nimport sys\n"
                   "who = ' '.join(sys.argv[1:]) or 'world'\n"
                   "print('Hello, %s - brewed by Vanbrew!' % who)\n"},
                   "as": "hello.py"}],
        "bin": [{"name": "hello", "kind": "python", "main": "hello.py"}],
    },
}

# ---- pretty output (degrades to plain on old/non-tty terminals) -------------
def _color_ok():
    if os.environ.get("NO_COLOR"):
        return False
    return hasattr(sys.stdout, "isatty") and sys.stdout.isatty() and not IS_WIN

_C = _color_ok()
def paint(s, code):
    return ("\033[%sm%s\033[0m" % (code, s)) if _C else s
def bold(s):  return paint(s, "1")
def green(s): return paint(s, "32")
def yellow(s): return paint(s, "33")
def cyan(s):  return paint(s, "36")
def red(s):   return paint(s, "31")
def dim(s):   return paint(s, "2")

def say(msg):  print(msg)
def ok(msg):   print(green("==> ") + msg)
def warn(msg): print(yellow("warning: ") + msg)
def die(msg):
    print(red("error: ") + msg, file=sys.stderr)
    sys.exit(1)

# ---- small helpers ----------------------------------------------------------
def which(cmd):
    for d in os.environ.get("PATH", "").split(os.pathsep):
        p = os.path.join(d, cmd)
        if os.path.isfile(p) and os.access(p, os.X_OK):
            return p
        if IS_WIN and os.path.isfile(p + ".exe"):
            return p + ".exe"
    return None

def python_exe():
    # what shims use to run .py files: prefer a stable `python3`, else this one
    return "python3" if which("python3") else (which("python") or sys.executable)

def ensure_dirs():
    for d in (VB_HOME, BIN, CELLAR, REG_DIR):
        if not os.path.isdir(d):
            os.makedirs(d)

def load_state():
    if os.path.isfile(STATE):
        try:
            with open(STATE) as f:
                return json.load(f)
        except ValueError:
            return {}
    return {}

def save_state(state):
    with open(STATE, "w") as f:
        json.dump(state, f, indent=2, sort_keys=True)

def _ingest(reg, data, fallback_name):
    # a registry file may be: one formula, a list of formulas, or a name->formula map
    items = []
    if isinstance(data, list):
        items = data
    elif isinstance(data, dict) and "files" in data:
        d = dict(data); d.setdefault("name", fallback_name); items = [d]
    elif isinstance(data, dict):
        for k, v in data.items():
            if isinstance(v, dict):
                d = dict(v); d.setdefault("name", k); items.append(d)
    for it in items:
        if it.get("name"):
            reg[it["name"]] = it

def load_registry():
    reg = {}
    for name, formula in BUILTIN.items():
        item = dict(formula)
        item["name"] = name
        reg[name] = item
    # formulae in ~/.vanbrew/registry/*.json override / extend the builtins
    if os.path.isdir(REG_DIR):
        for fn in sorted(os.listdir(REG_DIR)):
            if not fn.endswith(".json"):
                continue
            try:
                with open(os.path.join(REG_DIR, fn)) as f:
                    data = json.load(f)
            except ValueError:
                warn("skipping invalid formula file: " + fn)
                continue
            _ingest(reg, data, fn[:-5])
    return reg

def quote(p):
    return '"%s"' % p

# ---- fetching sources -------------------------------------------------------
def _http_bytes(url, timeout=60):
    """Download a URL. If TLS verification fails - common on iSH/Alpine and other
    systems missing CA certs - retry once without verification (with a warning) so
    install still works; the user can `apk add ca-certificates` for a proper fix."""
    req = _urlreq.Request(url, headers={"User-Agent": "Vanbrew/" + VERSION})
    try:
        return _urlreq.urlopen(req, timeout=timeout).read()
    except Exception as e:
        if "SSL" in type(e).__name__ or "CERTIFICATE" in str(e).upper():
            try:
                import ssl
                ctx = ssl._create_unverified_context()
                warn("TLS certificate check failed; retrying without it "
                     "(run 'apk add ca-certificates' to fix properly)")
                return _urlreq.urlopen(req, timeout=timeout, context=ctx).read()
            except Exception:
                pass
        raise

def fetch_source(src, target):
    kind = src.get("kind")
    if kind == "local":
        path = os.path.expanduser(os.path.expandvars(src["path"]))
        if not os.path.isfile(path):
            die("source file not found: %s\n       (set the right path via env "
                "or edit the formula)" % path)
        shutil.copyfile(path, target)
    elif kind == "inline":
        with open(target, "w") as f:
            f.write(src["text"])
    elif kind == "url":
        say(dim("    downloading " + src["url"]))
        data = _http_bytes(src["url"], timeout=60)
        if src.get("sha256"):
            import hashlib
            got = hashlib.sha256(data).hexdigest()
            if got != src["sha256"]:
                die("checksum mismatch for %s\n  expected %s\n  got      %s"
                    % (src["url"], src["sha256"], got))
        with open(target, "wb") as f:
            f.write(data)
    else:
        die("unknown source kind: %r" % kind)

# ---- shims ------------------------------------------------------------------
def shim_command(entry, celldir, state):
    main = os.path.join(celldir, entry["main"])
    kind = entry.get("kind", "exec")
    if kind == "python":
        return "%s %s" % (python_exe(), quote(main))
    if kind == "vanta":
        rec = state.get("vanta")
        if not rec:
            die("'%s' needs vanta - install it first (vanbrew install vanta)"
                % entry["name"])
        vanta_main = os.path.join(rec["path"], "vanta.py")
        return "%s %s %s" % (python_exe(), quote(vanta_main), quote(main))
    if kind == "shell":
        return "sh %s" % quote(main)
    if kind == "cc":
        # compile the shipped C source to a native binary at install time.
        # Needs only a C compiler - NO Python. This is how Vanta ships Python-free.
        celld = os.path.dirname(main)
        out = os.path.join(celld, entry["name"])
        stale = (not os.path.exists(out)) or os.path.getmtime(out) < os.path.getmtime(main)
        if stale:
            cc = which("cc") or which("clang") or which("gcc")
            if not cc:
                die("'%s' needs a C compiler (cc/clang/gcc). On macOS: xcode-select --install"
                    % entry["name"])
            rc = os.system("%s -O2 -w %s -o %s" % (cc, quote(main), quote(out)))
            if rc != 0 or not os.path.exists(out):
                die("failed to compile %s with cc" % entry["main"])
        try:
            os.chmod(out, 0o755)
        except OSError:
            pass
        return quote(out)
    # exec: the file itself is runnable
    try:
        os.chmod(main, os.stat(main).st_mode | stat.S_IEXEC | stat.S_IXGRP | stat.S_IXOTH)
    except OSError:
        pass
    return quote(main)

def write_shim(name, command):
    if IS_WIN:
        path = os.path.join(BIN, name + ".bat")
        with open(path, "w") as f:
            f.write("@echo off\r\n%s %%*\r\n" % command)
    else:
        path = os.path.join(BIN, name)
        with open(path, "w") as f:
            f.write("#!/bin/sh\nexec %s \"$@\"\n" % command)
        os.chmod(path, 0o755)
    return path

def remove_shim(name):
    for p in (os.path.join(BIN, name), os.path.join(BIN, name + ".bat")):
        if os.path.exists(p):
            os.remove(p)

# ---- commands ---------------------------------------------------------------
def _refresh_remote(reg):
    """Best-effort: pull the latest catalog from GitHub and merge it in, so a newly
    added package installs without a manual 'vanbrew update'. Stays quiet on failure
    (e.g. offline / flaky network on a phone) - the bundled catalog still works."""
    try:
        ensure_dirs()
        data = _http_bytes(REGISTRY_URL, timeout=30)
        with open(os.path.join(REG_DIR, "remote.json"), "wb") as f:
            f.write(data)
        _ingest(reg, json.loads(data.decode("utf-8")), "remote")
        return True
    except Exception:
        return False

def cmd_install(args):
    ensure_dirs()
    reg = load_registry()
    if any(n not in reg for n in args.names):   # name not bundled? try the latest catalog
        _refresh_remote(reg)
    state = load_state()
    seen = set()
    for name in args.names:
        _install_one(name, reg, state, seen)
    save_state(state)
    print()
    ok("done. Run " + bold("vanbrew list") + " to see what's installed.")
    if not _bin_on_path():
        _print_path_hint()

def _install_one(name, reg, state, seen):
    if name in seen:
        return
    seen.add(name)
    if name not in reg:
        die("no formula named '%s'. Try: vanbrew search %s" % (name, name))
    formula = reg[name]
    for dep in formula.get("deps", []):
        _install_one(dep, reg, state, seen)

    version = str(formula.get("version", "0"))
    celldir = os.path.join(CELLAR, name, version)
    if os.path.isdir(celldir):
        shutil.rmtree(celldir)
    os.makedirs(celldir)

    say(green("==> ") + "installing " + bold(name) + dim(" " + version))
    for f in formula.get("files", []):
        target = os.path.join(celldir, f["as"])
        fetch_source(f["source"], target)

    # record before making shims, so 'vanta'-kind deps can be looked up
    bins = [b["name"] for b in formula.get("bin", [])]
    state[name] = {"version": version, "path": celldir, "bins": bins,
                   "summary": formula.get("summary", "")}
    made = []
    for entry in formula.get("bin", []):
        command = shim_command(entry, celldir, state)
        write_shim(entry["name"], command)
        made.append(entry["name"])
    if made:
        say("    " + dim("commands: ") + ", ".join(cyan(m) for m in made))

def cmd_uninstall(args):
    state = load_state()
    for name in args.names:
        if name not in state:
            warn("'%s' is not installed" % name)
            continue
        for b in state[name].get("bins", []):
            remove_shim(b)
        pkgdir = os.path.join(CELLAR, name)
        if os.path.isdir(pkgdir):
            shutil.rmtree(pkgdir)
        del state[name]
        ok("removed " + bold(name))
    save_state(state)

def cmd_list(args):
    state = load_state()
    if not state:
        say(dim("nothing installed yet. Try: ") + bold("vanbrew install vanta"))
        return
    for name in sorted(state):
        rec = state[name]
        cmds = ", ".join(rec.get("bins", [])) or "-"
        print("%s %s   %s\n    %s" % (
            bold(name.ljust(12)), dim(rec.get("version", "")),
            dim("cmds: " + cmds), rec.get("summary", "")))

def cmd_search(args):
    reg = load_registry()
    q = args.query.lower()
    if q in (".", "*", ""):
        hits = list(reg.values())
    else:
        hits = [f for f in reg.values()
                if q in f["name"].lower() or q in f.get("summary", "").lower()]
    if not hits:
        say(dim("no formulae match '%s'" % args.query))
        return
    for f in sorted(hits, key=lambda x: x["name"]):
        print("%s %s\n    %s" % (
            bold(f["name"].ljust(12)), dim(str(f.get("version", ""))),
            f.get("summary", "")))

def cmd_info(args):
    reg = load_registry()
    if args.name not in reg:
        die("no formula named '%s'" % args.name)
    f = reg[args.name]
    state = load_state()
    print(bold(f["name"]) + " " + dim(str(f.get("version", ""))))
    print("  " + f.get("summary", ""))
    if f.get("deps"):
        print("  " + dim("deps:     ") + ", ".join(f["deps"]))
    if f.get("bin"):
        print("  " + dim("commands: ") + ", ".join(b["name"] for b in f["bin"]))
    print("  " + dim("status:   ") +
          (green("installed " + state[f["name"]]["version"])
           if f["name"] in state else "not installed"))

def cmd_update(args):
    ensure_dirs()
    try:
        data = _http_bytes(REGISTRY_URL, timeout=30)
        with open(os.path.join(REG_DIR, "remote.json"), "wb") as f:
            f.write(data)
        ok("fetched the latest catalog from GitHub")
    except Exception as e:
        warn("couldn't fetch the remote catalog (%s) - using what's bundled" % e)
    reg = load_registry()
    ok("%d formulae available" % len(reg))
    say(dim("    add your own by dropping a .json formula into " + REG_DIR))

def cmd_doctor(args):
    ensure_dirs()
    print(bold("Vanbrew doctor"))
    pv = "%d.%d.%d" % sys.version_info[:3]
    print("  python:        " + pv + "  " + (green("ok") if sys.version_info[0] >= 2 else red("too old")))
    print("  python3 shim:  " + (green(python_exe())))
    print("  vanbrew home:  " + VB_HOME)
    on_path = _bin_on_path()
    print("  bin on PATH:   " + (green("yes") if on_path else yellow("no")))
    state = load_state()
    print("  installed:     " + (", ".join(sorted(state)) or dim("none")))
    if not on_path:
        print()
        _print_path_hint()

def cmd_setup(args):
    ensure_dirs()
    print(bold(cyan(_BANNER)))
    # install Vanbrew itself so `vanbrew` works from anywhere
    celldir = os.path.join(CELLAR, "vanbrew", VERSION)
    if os.path.isdir(celldir):
        shutil.rmtree(celldir)
    os.makedirs(celldir)
    shutil.copyfile(SELF, os.path.join(celldir, "vanbrew.py"))
    write_shim("vanbrew", "%s %s" % (python_exe(), quote(os.path.join(celldir, "vanbrew.py"))))
    state = load_state()
    state["vanbrew"] = {"version": VERSION, "path": celldir, "bins": ["vanbrew"],
                        "summary": "the Vanbrew package manager itself"}
    save_state(state)
    # drop a sample custom formula so people see how to add their own
    sample = os.path.join(REG_DIR, "example.json")
    if not os.path.isfile(sample):
        with open(sample, "w") as f:
            json.dump({
                "name": "example",
                "version": "1.0",
                "summary": "a sample custom formula - edit me or copy me",
                "files": [{"source": {"kind": "inline",
                           "text": "#!/usr/bin/env python3\nprint('your package here')\n"},
                           "as": "example.py"}],
                "bin": [{"name": "example", "kind": "python", "main": "example.py"}],
            }, f, indent=2)
    ok("Vanbrew installed to " + VB_HOME)
    added = _add_to_profiles()
    print()
    ok(bold("Next steps:"))
    if added:
        print("  1. Restart your terminal (or run: " + cyan("source ~/.zshrc") + ")")
    else:
        _print_path_hint()
        print("  1. Add the line above to your shell profile, then restart it")
    print("  2. " + cyan("vanbrew install vanta") + "   # then use 'vanta file.va' anywhere")
    print("  3. " + cyan("vanbrew install topdeck") + " # then just run 'topdeck'")
    print("  4. " + cyan("vanbrew search .") + "         # browse everything available")

# ---- PATH integration -------------------------------------------------------
def _bin_on_path():
    parts = [os.path.normpath(p) for p in os.environ.get("PATH", "").split(os.pathsep)]
    return os.path.normpath(BIN) in parts

def _print_path_hint():
    print(yellow("Add Vanbrew to your PATH so its commands work everywhere:"))
    if IS_WIN:
        print("  setx PATH \"%%PATH%%;" + BIN + "\"")
    else:
        print("  " + cyan('export PATH="%s:$PATH"' % BIN))

PROFILE_MARK = "# >>> vanbrew >>>"
def _add_to_profiles():
    if IS_WIN:
        return False
    line = ('\n%s\nexport PATH="%s:$PATH"\n# <<< vanbrew <<<\n'
            % (PROFILE_MARK, BIN))
    touched = False
    candidates = [os.path.join(HOME, ".zshrc"), os.path.join(HOME, ".bashrc"),
                  os.path.join(HOME, ".profile")]
    # prefer existing files; if none exist, create ~/.zshrc (default mac shell)
    existing = [p for p in candidates if os.path.isfile(p)] or [candidates[0]]
    for prof in existing:
        try:
            content = ""
            if os.path.isfile(prof):
                with open(prof) as f:
                    content = f.read()
            if PROFILE_MARK in content:
                touched = True
                continue
            with open(prof, "a") as f:
                f.write(line)
            ok("added Vanbrew to PATH in " + prof)
            touched = True
        except (IOError, OSError):
            pass
    return touched

_BANNER = r"""
 __   __         _
 \ \ / /__ _ _ _| |__ _ _ _____ __ __
  \ V / _` | ' \ '_ \ '_/ -_) V V /
   \_/\__,_|_||_.__/_| \___|\_/\_/   pip + Homebrew, in one file
"""

# ---- arg parsing ------------------------------------------------------------
def build_parser():
    import argparse
    p = argparse.ArgumentParser(
        prog="vanbrew",
        description="Vanbrew - a tiny universal package manager (pip + Homebrew in one).")
    p.add_argument("--version", action="version", version="vanbrew " + VERSION)
    sub = p.add_subparsers(dest="cmd")

    s = sub.add_parser("setup", help="first-time install of Vanbrew itself + PATH")
    s.set_defaults(func=cmd_setup)

    s = sub.add_parser("install", help="install one or more packages")
    s.add_argument("names", nargs="+")
    s.set_defaults(func=cmd_install)

    s = sub.add_parser("uninstall", help="remove one or more packages")
    s.add_argument("names", nargs="+")
    s.set_defaults(func=cmd_uninstall)

    s = sub.add_parser("list", help="list installed packages")
    s.set_defaults(func=cmd_list)

    s = sub.add_parser("search", help="search the catalog ('.' for everything)")
    s.add_argument("query")
    s.set_defaults(func=cmd_search)

    s = sub.add_parser("info", help="show details for a package")
    s.add_argument("name")
    s.set_defaults(func=cmd_info)

    s = sub.add_parser("update", help="reload the catalog of formulae")
    s.set_defaults(func=cmd_update)

    s = sub.add_parser("doctor", help="check your setup")
    s.set_defaults(func=cmd_doctor)
    return p

def main(argv):
    parser = build_parser()
    args = parser.parse_args(argv)
    if not getattr(args, "cmd", None):
        parser.print_help()
        return 0
    args.func(args)
    return 0

if __name__ == "__main__":
    try:
        sys.exit(main(sys.argv[1:]))
    except KeyboardInterrupt:
        sys.exit(130)
