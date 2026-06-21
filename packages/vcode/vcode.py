#!/usr/bin/env python3
# vanta-code - a terminal coding agent that specializes in the Vanta language,
# styled to look like Claude Code. Uses YOUR OWN API key (Anthropic or
# OpenRouter) from the environment. Single file, standard library only.
from __future__ import print_function
import os, sys, json, time, threading, subprocess, shutil, tempfile, re, difflib
import urllib.request, urllib.error

VERSION = "4.0"

# ---------------------------------------------------------------- colours ----
COLOR = sys.stdout.isatty() and os.environ.get("TERM") not in (None, "", "dumb")
def _c(s, code):
    return ("\033[%sm%s\033[0m" % (code, s)) if COLOR else s
DIM    = "2;37"
GREY   = "38;2;140;140;150"
GREEN  = "38;2;126;192;80"
RED    = "38;2;229;90;90"
BLUE   = "38;2;120;160;255"
BOLD   = "1"

# Swappable colour themes: each sets the accent (used everywhere as orange())
# and the 5-stop wordmark gradient. Change live with /themes.
THEMES = {
    "ember":     {"accent": (217, 119, 87),  "grad": [(99,102,241),(168,85,247),(217,70,160),(240,118,92),(245,176,86)]},
    "synthwave": {"accent": (255, 92, 170),  "grad": [(99,72,255),(173,66,235),(255,72,180),(255,120,96),(255,196,92)]},
    "matrix":    {"accent": (90, 220, 130),  "grad": [(20,110,55),(46,190,96),(120,236,150),(196,255,205),(80,214,128)]},
    "ice":       {"accent": (96, 180, 255),  "grad": [(64,96,210),(86,160,255),(128,220,255),(190,242,255),(120,200,255)]},
    "gold":      {"accent": (240, 184, 84),  "grad": [(120,64,24),(200,120,44),(245,182,84),(255,224,150),(244,176,80)]},
    "mono":      {"accent": (224, 224, 232), "grad": [(96,96,108),(150,150,162),(208,208,220),(244,244,248),(176,176,190)]},
}
THEME = {"name": "ember", "accent": THEMES["ember"]["accent"], "grad": list(THEMES["ember"]["grad"])}
def set_theme(name):
    if name not in THEMES: return False
    THEME["name"] = name
    THEME["accent"] = THEMES[name]["accent"]
    THEME["grad"] = list(THEMES[name]["grad"])
    return True

def _code(rgb): return "38;2;%d;%d;%d" % tuple(rgb)
def orange(s): return _c(s, _code(THEME["accent"]))   # "orange" = the active theme accent
def dim(s):    return _c(s, DIM)
def grey(s):   return _c(s, GREY)
def green(s):  return _c(s, GREEN)
def red(s):    return _c(s, RED)
def blue(s):   return _c(s, BLUE)
def bold(s):   return _c(s, BOLD)

# ----------------------------------------------------- the VANTA wordmark ----
# ANSI Shadow block letters, swept by a horizontal "vantablack dusk" gradient.
VANTA_ART = [
    "██╗   ██╗ █████╗ ███╗   ██╗████████╗ █████╗ ",
    "██║   ██║██╔══██╗████╗  ██║╚══██╔══╝██╔══██╗",
    "██║   ██║███████║██╔██╗ ██║   ██║   ███████║",
    "╚██╗ ██╔╝██╔══██║██║╚██╗██║   ██║   ██╔══██║",
    " ╚████╔╝ ██║  ██║██║ ╚████║   ██║   ██║  ██║",
    "  ╚═══╝  ╚═╝  ╚═╝╚═╝  ╚═══╝   ╚═╝   ╚═╝  ╚═╝",
]
def _lerp(a, b, t): return tuple(int(a[k] + (b[k] - a[k]) * t) for k in range(3))
def _at(stops, t):
    if t <= 0: return stops[0]
    if t >= 1: return stops[-1]
    seg = t * (len(stops) - 1); i = int(seg); return _lerp(stops[i], stops[i + 1], seg - i)
def _grad_at(t): return _at(THEME["grad"], t)
def grad_line(line):
    if not COLOR: return line
    n = max(1, len(line) - 1); out = []; last = None
    for i, ch in enumerate(line):
        if ch == " ": out.append(ch); last = None; continue
        code = "\033[38;2;%d;%d;%dm" % _grad_at(i / float(n))
        if code != last: out.append(code); last = code
        out.append(ch)
    out.append("\033[0m"); return "".join(out)

def term_width():
    try: return max(24, min(shutil.get_terminal_size().columns, 90))   # floor for narrow phones
    except Exception: return 80

# --------------------------------------------------------------- spinner -----
SPIN_WORDS = ["Thinking", "Cogitating", "Noodling", "Percolating", "Vibing",
              "Brewing", "Schlepping", "Pondering", "Conjuring", "Compiling thoughts"]
BRAILLE = "⠋⠙⠹⠸⠼⠴⠦⠧⠇⠏"
class Spinner(object):
    def __init__(self, word):
        self.word = word; self._stop = False; self._t = None; self.t0 = time.time()
    def start(self):
        if not COLOR: return self
        self._t = threading.Thread(target=self._run); self._t.daemon = True; self._t.start(); return self
    def _run(self):
        i = 0
        while not self._stop:
            el = int(time.time() - self.t0)
            frame = BRAILLE[i % len(BRAILLE)]
            line = "%s %s%s %s" % (orange(frame), orange(self.word + "…"),
                                   dim(" (%ss" % el), dim("· ctrl-c to interrupt)"))
            sys.stdout.write("\r\033[K" + line); sys.stdout.flush()
            time.sleep(0.08); i += 1
    def stop(self):
        self._stop = True
        if self._t: self._t.join(timeout=0.3)
        if COLOR: sys.stdout.write("\r\033[K"); sys.stdout.flush()

# ------------------------------------------------------------ vanta knowledge -
SYSTEM = """You are Vanta Code, a focused terminal coding agent that specializes in the Vanta programming language. You help the user read, write, run, and debug Vanta (.va) programs. Be concise and direct, like a senior pair-programmer in a terminal. Prefer doing over explaining: use your tools to read files, write code, and run it to verify.

# Vanta in a nutshell (it reads like plain English)
- Output: `say <expr>`. Strings join with `+`. Numbers -> text with `text(x)`; text -> number with `number(s)`.
- Variables: `let x be <expr>` to create, `change x to <expr>` to reassign.
- Functions: `to greet(name)` ... `end`, return a value with `give back <expr>`. Call as `greet("Sam")`.
- Conditionals: `if <cond>` / `otherwise if <cond>` / `otherwise` / `end`. Comparisons in words: `is`, `is not`, `is at least`, `is at most`, `is over`, `is under`. Combine with `and`, `or`, `not`. Membership/substring: `x is in y`.
- Loops: `for each item in <list>` ... `end`; `while <cond>` ... `end`; numeric ranges via `range(a, b)`.
- Lists: `let xs be []`, `add 3 to xs`, index `xs[0]`, `length(xs)`. Maps: `let m be {"k": 1}`, read `m["k"]`, set `change m at "k" to 2`, `keys(m)`.
- Strings: `uppercase(s)`, `lowercase(s)`, `trim(s)`, `slice(s, a, b)`, `replace(s, old, new)`, `split(s, sep)`, `join(list, sep)`, `starts_with(s, pre)`, `ends_with(s, suf)`, `find(s, sub)` (index or -1), `contains(s, sub)`, `reverse`, `length(s)`. NOTE the case builtins are `uppercase`/`lowercase` — there is NO `upper`/`lower`.
- String interpolation: `"hi {name}"` inserts the value of name. To put a LITERAL brace in a string, double it: `{{` and `}}` (this matters a LOT when emitting CSS/JS).
- Web + system builtins: `serve(port, handler)` (handler takes a request map, gives back text or a map {status, body, type, headers}); `http_get(url[, headers])` and `http_post(url, body[, headers])` -> {status, body, headers}; `read_file`/`write_file`/`append_file`/`list_dir`/`make_dir`/`remove_path`; `from_json`/`to_json`; `run(cmd)` and `shell(cmd)`; `open_url(url)`; `home_dir()`, `path_join(...)`, `dirname`, `basename`; `now()`, `today()`, `clock()`; `run_vanta(code)` runs Vanta source in-process; `ask(prompt)` reads a line of input; `os_name()`; `typeof(x)` -> "string"/"number"/"bool"/"list"/"map"/"nothing".
- Every block (`if`, `for each`, `while`, `to`) closes with `end`. There are no curly-brace code blocks and no semicolons. `times` is reserved - don't use it as a variable.

# Running Vanta natively, with NO Python (vc + vself)
Vanta is self-hosting: it can run and compile itself with zero Python at runtime (only a C compiler). The user installs these via `vanbrew install vc vself`:
- `vself prog.va` — a native Vanta INTERPRETER (written in Vanta, compiled to a native binary). Runs scripts directly AND web servers (`serve()`), no compile step, no Python.
- `vc prog.va` — a native Vanta-to-C COMPILER. Compiles `prog.va` to a native binary (`prog.va.bin`) and runs it. Supports strings/lists/maps/`serve()`/HTTP/JSON/filesystem, with a garbage collector. `vc prog.va -c` compiles only (no run); `vc prog.va -k` emits freestanding kernel C.
So for "make this run without Python" / "compile to a native binary" / "ship a standalone binary": use `vc` (via the bash tool). The runtime is POSIX C — builds on macOS and Linux (CI-verified). `run_vanta`/`run_app` (your tools) use the Python `vanta` interpreter for quick dev output; `vc`/`vself` are the native, Python-free path.

# How to work
- FINISH THE JOB IN ONE TURN. When asked to build something, do the WHOLE task now: make the folder AND write every file with its full contents AND launch it — all in this one turn, using as many tool calls as it takes. Do NOT stop after announcing a plan, and do NOT stop after just make_dir. Never end your turn with the work half-done and never ask the user to say "continue" — only stop when the app is actually written and runnable (or you truly need a decision from the user). After a make_dir your very next action must be write_file for the real code.
- You have FULL ACCESS to this computer. You can create folders ANYWHERE (make_dir), read/write/move/delete any file, and run any shell command (bash). You are NOT limited to the current directory - use absolute paths (e.g. ~/projects/foo/app.va, /Users/.../). Coding Vanta works in any location.
- Your main job is BUILDING FROM SCRATCH. When the user asks you to make / build / create / write / code an app or program, WRITE it yourself with write_file - produce complete, original, working Vanta code. Do NOT reuse, copy, or just run a file that already exists, and do NOT go hunting for an existing .va to run. "Make a tip calculator" means write brand-new .va code for one - never run ~/tipjar.va or anything pre-made unless the user EXPLICITLY says "run the existing X".
- For a new project, MAKE A DEDICATED FOLDER for it (make_dir, e.g. ~/vanta/<name>/) and put the .va plus any assets inside, unless the user says where. Then launch it so the user sees it: a visual/web app -> run_app (pops a movable window); a plain script -> run_vanta (console output). Read any error, fix the .va, and re-run until it works.
- Only use run_app/run_vanta on an EXISTING file when the user explicitly says "run/open <that file>". Otherwise you are creating, not fetching.
- To CHANGE an existing file, use edit_file (replace exact 'old' text with 'new') — it is surgical and shows a clean diff. Use write_file only for brand-new files or full rewrites. Use search (grep contents) and glob (find files by name) to locate code before editing.
- To COPY or duplicate a file (e.g. clone an app into a new folder), DO NOT re-type its contents into write_file — that wastes tokens and can hit the output limit on big files. Instead use bash to copy it (`cp source dest`, after make_dir for the folder), then edit_file on the copy for the small changes (renames, paths, branding). Reserve full write_file for genuinely new files.
- Keep answers tight: a sentence on what you built, then the result. You may use light markdown (**bold**, `code`, # headings, ```fenced``` blocks); it renders in the terminal.

# Building a visual app in Vanta (write this from scratch)
A Vanta GUI/web app builds an HTML page as a string, writes it to a file, and opens it. CRITICAL: in Vanta strings a single { } means interpolation, so write `{{` and `}}` for every literal brace in CSS/JS. Use single quotes for all HTML attributes so you never escape double quotes. Working skeleton (a draggable card) - adapt the UI and logic to whatever the user asked for:

let html be "<!doctype html><html><head><meta charset='utf-8'><style>"
change html to html + "body{{margin:0;height:100vh;font-family:system-ui;background:#0b1020;color:#eef}}"
change html to html + ".card{{position:fixed;left:120px;top:120px;width:300px;padding:22px;border-radius:18px;background:#182038;box-shadow:0 20px 60px rgba(0,0,0,.5)}}"
change html to html + ".bar{{cursor:grab;font-weight:700;margin-bottom:14px}}"
change html to html + "</style></head><body>"
change html to html + "<div class='card' id='card'><div class='bar' id='bar'>My App</div><div id='body'>build the UI here</div></div>"
change html to html + "<script>"
change html to html + "var card=document.getElementById('card'),bar=document.getElementById('bar');"
change html to html + "bar.addEventListener('mousedown',function(e){{var sx=e.clientX,sy=e.clientY,ox=card.offsetLeft,oy=card.offsetTop;function mv(ev){{card.style.left=(ox+ev.clientX-sx)+'px';card.style.top=(oy+ev.clientY-sy)+'px';}}function up(){{document.removeEventListener('mousemove',mv);document.removeEventListener('mouseup',up);}}document.addEventListener('mousemove',mv);document.addEventListener('mouseup',up);}});"
change html to html + "</script></body></html>"
let dest be path_join(home_dir(), "myapp.html")
write_file(dest, html)
open_url("file://" + dest)
say "opened"

Put real inputs/buttons in <div id='body'> and their logic in the <script> (use `{{`/`}}` for braces).

PREFER this file pattern (write HTML -> open_url) for visual apps: it has NO port and never clashes with anything. Use serve() ONLY when you truly need a live backend, and then pick an UNCOMMON HIGH PORT like 8765 - NEVER 8080, 8090, or 8100 (the user already runs apps there, e.g. a conlang site on 8080; opening those shows the wrong app). If run_app says a port is busy, change to another free high port and run_app again.

# Building a GAME in Vanta (the vanta-game engine)
Vanta can make real 2D games that compile to a NATIVE binary (no Python) and run in a window via SDL. A game is a .va file using the game API below; it's built from the vanta-game project with `./build.sh <name>` then run with `./<name>` (needs SDL2 + the project's sdlrt.c runtime — see github.com/Juanshep1/vanta-game). The game loop:

background(rgb(12, 14, 32))
while quit() is 0
    poll()
    if held("left") is 1
        change px to px - 6
    end
    clear()
    rfill(px, py, 40, 40, rgb(94, 240, 200), 8)
    present()
    delay(16)
end

Game API: `screen_w()` `screen_h()`; `rgb(r,g,b)`; `background(c)`; `clear()`; `present()`; `fill(x,y,w,h,c)`; `rfill(x,y,w,h,c,radius)`; `circle(x,y,r,c)`; `line(x0,y0,x1,y1,c)`; `rect(x,y,w,h,c)`; `text_at`/`text_big`/`text_huge(x,y,s,c)`; `poll()`; `held("left"|"right"|"up"|"down"|"space"|"a".."z")` -> 1/0; `pressed(name)` -> 1 only the frame a key goes down; `key()` -> last typed char; `mouse_x()` `mouse_y()` `mouse_down()`; `sound(freq,ms)`; `random(n)` `random_range(a,b)`; `quit()` -> 1 when the window is closed; `ticks()`; `delay(ms)`; `title(s)`. Keep state in lists/maps and grow them freely — the game runtime has a garbage collector. Numbers are integers (no floats). Do NOT use serve()/run_app for games; they're native windowed apps built with build.sh."""

_CTX = {"text": ""}   # project context (VANTA.md / AGENTS.md / CLAUDE.md), loaded at startup
USAGE = {"ctx": 0, "out": 0}   # last input (context) tokens + cumulative output tokens
SESSION = {"start": None, "tools": 0, "turns": 0}   # /cost stats for this run
MAX_TOKENS = 16384             # output cap per turn (8192 truncated big file writes)

def _human(n):
    return ("%.1fk" % (n / 1000.0)) if n >= 1000 else str(int(n))

VKW = set("let be to end if otherwise while for each in give back change say add and or "
          "not is at least most over under range text number length keys serve".split())
def _hl_code(line):
    # light Vanta syntax highlighting for fenced code blocks in replies
    if not COLOR: return "  | " + line
    out = []; i = 0; n = len(line)
    while i < n:
        c = line[i]
        if c == "#": out.append(green(line[i:])); break              # comment to EOL
        if c == '"':                                                  # string literal
            j = i + 1
            while j < n and line[j] != '"': j += 1
            out.append(blue(line[i:j + 1])); i = j + 1; continue
        if c.isalpha() or c == "_":                                   # word / keyword
            j = i
            while j < n and (line[j].isalnum() or line[j] == "_"): j += 1
            w = line[i:j]; out.append(orange(w) if w in VKW else w); i = j; continue
        out.append(c); i += 1
    return dim("  │ ") + "".join(out)

def session_path(): return os.path.expanduser("~/.vanta-code/last_session.json")
def save_session(history):
    try:
        os.makedirs(os.path.dirname(session_path()), exist_ok=True)
        json.dump(history, open(session_path(), "w"))
    except Exception: pass
def load_session():
    try: return json.load(open(session_path()))
    except Exception: return None
def load_project_context():
    chunks = []
    for nm in ("VANTA.md", "AGENTS.md", "CLAUDE.md"):
        for base in (os.getcwd(), os.path.expanduser("~/.vanta-code")):
            fp = os.path.join(base, nm)
            if os.path.isfile(fp):
                try: chunks.append("# Project context (%s)\n%s" % (nm, open(fp, "r", errors="replace").read()[:6000]))
                except Exception: pass
                break
    return ("\n\n" + "\n\n".join(chunks)) if chunks else ""

# ------------------------------------------------------------------ skills ---
# A Skill is a folder with a SKILL.md: YAML frontmatter (name, description) + a
# markdown body of instructions, optionally bundling scripts/files. This is the
# SAME format as Claude Code's Agent Skills, so vcode picks up skills you already
# have in ~/.claude/skills/ as well as ~/.vanta-code/skills/.
_SKILLS = {}   # name -> {name, description, file, dir, body}

def _skill_dirs():
    out = []
    for base in (os.getcwd(), os.path.expanduser("~")):
        for sub in (".vanta-code/skills", ".claude/skills"):
            out.append(os.path.join(base, sub))
    return out

def _parse_skill(path):
    try:
        txt = open(path, "r", errors="replace").read()
    except Exception:
        return None
    fields = {}; body = txt
    m = re.match(r"^---\s*\n(.*?)\n---\s*\n?(.*)$", txt, re.S)
    if m:
        body = m.group(2)
        lines = m.group(1).splitlines(); i = 0
        while i < len(lines):
            kv = re.match(r"^([A-Za-z_][\w-]*):\s*(.*)$", lines[i])
            if kv:
                k = kv.group(1).strip().lower(); v = kv.group(2).strip()
                if v in ("|", "|-", "|+", ">", ">-", ">+"):     # YAML block scalar
                    parts = []; i += 1
                    while i < len(lines) and (lines[i].startswith((" ", "\t")) or not lines[i].strip()):
                        parts.append(lines[i].strip()); i += 1
                    fields[k] = " ".join(p for p in parts if p); continue
                fields[k] = v.strip('"').strip("'")
            i += 1
    name = fields.get("name") or os.path.basename(os.path.dirname(path))
    return {"name": name, "description": fields.get("description", ""),
            "file": path, "dir": os.path.dirname(path), "body": body}

def install_skills(repo="https://github.com/anthropics/skills"):
    """git-clone a skills repo and copy every folder with a SKILL.md into
    ~/.vanta-code/skills/. Returns (count, dest_or_error)."""
    import tempfile, shutil
    if re.match(r"^[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+$", repo) and not os.path.exists(repo):
        repo = "https://github.com/" + repo            # owner/name shorthand
    dest = os.path.expanduser("~/.vanta-code/skills"); os.makedirs(dest, exist_ok=True)
    tmp = tempfile.mkdtemp()
    try:
        r = subprocess.run(["git", "clone", "--depth", "1", "-q", repo, tmp],
                           stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=180)
        if r.returncode != 0:
            shutil.rmtree(tmp, ignore_errors=True)
            return 0, (r.stderr.decode("utf-8", "replace").strip() or "git clone failed")
    except Exception as e:
        shutil.rmtree(tmp, ignore_errors=True); return 0, str(e)
    n = 0
    for root, dirs, files in os.walk(tmp):
        dirs[:] = [d for d in dirs if not d.startswith(".")]   # skip .git, .gemini dup dirs
        if "SKILL.md" in files:
            nm = os.path.basename(root)
            if nm not in ("", ".", "template"):
                try:
                    shutil.rmtree(os.path.join(dest, nm), ignore_errors=True)
                    shutil.copytree(root, os.path.join(dest, nm)); n += 1
                except Exception: pass
            dirs[:] = []                                # don't recurse into a skill folder
    shutil.rmtree(tmp, ignore_errors=True)
    return n, dest

def discover_skills():
    _SKILLS.clear()
    for d in _skill_dirs():
        if not os.path.isdir(d): continue
        for entry in sorted(os.listdir(d)):
            sf = os.path.join(d, entry, "SKILL.md")
            if os.path.isfile(sf):
                sk = _parse_skill(sf)
                if sk and sk["name"] not in _SKILLS:   # first wins: project > user
                    _SKILLS[sk["name"]] = sk
    return _SKILLS

# ---- My Skills: a small curated set the user pins from the big /skills list,
# persisted so they're one keystroke away (in /myskills and the / menu). --------
_MYSKILLS = []   # pinned skill names, newest-last
def _myskills_path(): return os.path.expanduser("~/.vanta-code/myskills.json")

def load_myskills():
    try:
        with open(_myskills_path()) as f:
            data = json.load(f)
        if isinstance(data, list):
            _MYSKILLS[:] = [n for n in data if isinstance(n, str)]
    except Exception:
        pass
    return _MYSKILLS

def save_myskills():
    try:
        os.makedirs(os.path.dirname(_myskills_path()), exist_ok=True)
        with open(_myskills_path(), "w") as f:
            json.dump(_MYSKILLS, f, indent=2)
        return True
    except Exception:
        return False

def add_myskill(name):
    if name in _MYSKILLS: return False
    _MYSKILLS.append(name); save_myskills(); return True

def remove_myskill(name):
    if name in _MYSKILLS:
        _MYSKILLS.remove(name); save_myskills(); return True
    return False

def my_skill_names():
    # only those still installed, in the order they were pinned
    return [n for n in _MYSKILLS if n in _SKILLS]

def skills_prompt():
    # Scales to any number of skills: with a handful, list name+description; with
    # many (a big installed pile), list NAMES only + a find_skill hint, so the
    # context stays lean instead of ballooning by ~5k tokens.
    if not _SKILLS: return ""
    n = len(_SKILLS)
    out = ["\n\n# Skills available (reusable expertise; Claude-Code compatible)"]
    if n <= 25:
        out.append("When a task matches a Skill, call `use_skill` with its name FIRST to load its full instructions, then follow them. A skill folder may bundle scripts/files - read or run them as it says.")
        for s in _SKILLS.values():
            d = " ".join((s["description"] or "(no description)").split())
            if len(d) > 200: d = d[:197] + "..."
            out.append("- **%s**: %s" % (s["name"], d))
    else:
        out.append("You have %d Skills installed. To use one, call `use_skill(name)`; if you're not sure which fits a task, call `find_skill(\"keyword\")` to search them by name/description. Available skill names:" % n)
        out.append(", ".join(_SKILLS.keys()))
    return "\n".join(out)

_EXAMPLE_SKILLS = {
"vanta-web-app": """---
name: vanta-web-app
description: Build a web or GUI app in the Vanta language - the {{ }} brace rule, the build-HTML-string-then-open pattern, and serve() for real backends. Use when asked to make a Vanta web app, dashboard, tool, or visual program.
---

# Building a web/GUI app in Vanta
A Vanta visual app builds an HTML page as a STRING, writes it to a file, opens it.

## The rule that trips everyone up
In a Vanta string a single `{` means interpolation, so DOUBLE every literal brace
in CSS/JS: write `{{` and `}}`. Use single quotes for HTML attributes.

```
to page()
    let css be "body{{font:16px system-ui;background:#0e1020;color:#eee;padding:40px}}"
    give back "<!doctype html><html><head><style>" + css + "</style></head><body><h1>Hi</h1></body></html>"
end
write_file("/tmp/app.html", page())
open_url("/tmp/app.html")
```

## When you need a backend
`serve(port, handle)` on an UNCOMMON high port (8765, never 8080/8090/8100). The
handler takes `{method,path,query,headers,body}` and gives back a string OR a map
`{status, body, type, headers}` (map/list body auto-JSONs).

Make a dedicated folder, use the file pattern for visual apps, serve() only for
live data, then run_app to show it. Join strings with `+`; `text(x)` to stringify.
""",
"vanta-game": """---
name: vanta-game
description: Build a native 2D game in Vanta that compiles to a real binary via the SDL engine (vc -k + sdlrt.c). Use when asked to make a Vanta game - graphics, input, sound, sprites, a game loop.
---

# Building a native game in Vanta
Games compile to a NATIVE binary via `vc -k` + the SDL runtime (github.com/Juanshep1/vanta-game).
Build with `./build.sh <name>` then run `./<name>` (needs SDL2 + sdlrt.c).

## The loop
```
title("My Game")
window(560, 480)
background(rgb(14, 16, 30))
let x be 100
while quit() is 0
    poll()
    if held("right") is 1
        change x to x + 6
    end
    clear()
    rfill(x, 200, 40, 40, rgb(94, 240, 200), 8)
    present()
    delay(16)
end
```

## API
- `rgb(r,g,b)`; `window(w,h)`; `background(c)`; `clear()`/`present()`; `delay(16)`.
- Draw: `fill` `rfill(x,y,w,h,c,radius)` `circle` `line` `rect` `text_at`/`text_big`/`text_huge` `sprite(x,y,rows,scale,palette)`.
- Input: `poll()`; `held("left/right/up/down/space/a..z")`->1/0; `pressed(name)` (edge); `key()`; `mouse_x/y/down()`.
- `sound(freq,ms)`; `random(n)`/`random_range(a,b)`; `quit()`->1 on close; `ticks()`.
Keep state in lists/maps (the runtime has a GC). Do NOT use serve()/run_app for games.
""",
}

def _seed_skills():
    base = os.path.expanduser("~/.vanta-code/skills")
    try:
        if os.path.isdir(base) and any(os.path.isdir(os.path.join(base, d)) for d in os.listdir(base)):
            return                               # user already has skills - don't touch
        for nm, body in _EXAMPLE_SKILLS.items():
            d = os.path.join(base, nm); os.makedirs(d, exist_ok=True)
            f = os.path.join(d, "SKILL.md")
            if not os.path.exists(f):
                open(f, "w").write(body)
    except Exception:
        pass

def refresh_context():
    discover_skills()
    _CTX["text"] = load_project_context() + skills_prompt()

# ------------------------------------------------------------------- tools ---
TOOLS = [
    {"name": "read_file", "description": "Read a UTF-8 text file and return its contents.",
     "input_schema": {"type": "object", "properties": {"path": {"type": "string"}}, "required": ["path"]}},
    {"name": "write_file", "description": "Write (create or overwrite) a whole text file. Use for new files or full rewrites.",
     "input_schema": {"type": "object", "properties": {"path": {"type": "string"}, "content": {"type": "string"}}, "required": ["path", "content"]}},
    {"name": "edit_file", "description": "Make a surgical edit to an existing file: replace the exact text 'old' with 'new' (old must appear exactly once). Prefer this over write_file for small changes.",
     "input_schema": {"type": "object", "properties": {"path": {"type": "string"}, "old": {"type": "string"}, "new": {"type": "string"}}, "required": ["path", "old", "new"]}},
    {"name": "search", "description": "Search file contents for a regex/text pattern (like grep). Returns file:line:match.",
     "input_schema": {"type": "object", "properties": {"pattern": {"type": "string"}, "path": {"type": "string"}}, "required": ["pattern"]}},
    {"name": "glob", "description": "Find files by name pattern (e.g. *.va) under a directory.",
     "input_schema": {"type": "object", "properties": {"pattern": {"type": "string"}, "path": {"type": "string"}}, "required": ["pattern"]}},
    {"name": "list_files", "description": "List files in a directory (defaults to the current directory). Pass any absolute path to look anywhere on the computer.",
     "input_schema": {"type": "object", "properties": {"path": {"type": "string"}}}},
    {"name": "make_dir", "description": "Create a folder (and any parent folders) anywhere on the computer. Use absolute paths like ~/projects/myapp or /Users/.../foo.",
     "input_schema": {"type": "object", "properties": {"path": {"type": "string"}}, "required": ["path"]}},
    {"name": "move_path", "description": "Move or rename a file/folder.",
     "input_schema": {"type": "object", "properties": {"from": {"type": "string"}, "to": {"type": "string"}}, "required": ["from", "to"]}},
    {"name": "delete_path", "description": "Delete a file or folder (recursively). Asks the user to confirm.",
     "input_schema": {"type": "object", "properties": {"path": {"type": "string"}}, "required": ["path"]}},
    {"name": "run_vanta", "description": "Run a Vanta .va file and return its TEXT/console output. Use only for non-visual scripts. Do NOT use on serve() web apps (they run forever) or visual apps (use run_app instead).",
     "input_schema": {"type": "object", "properties": {"path": {"type": "string"}}, "required": ["path"]}},
    {"name": "run_app", "description": "Run a Vanta PROJECT and pop up its window. Use this whenever the user wants to run / open / launch / show / see a Vanta app (web apps, the tip calculator, any visual program). Web pages open in a movable, draggable app-window; serve() apps are launched and opened at their port. This is the right tool for 'run the tip calculator'.",
     "input_schema": {"type": "object", "properties": {"path": {"type": "string"}}, "required": ["path"]}},
    {"name": "bash", "description": "Run a shell command and return stdout/stderr.",
     "input_schema": {"type": "object", "properties": {"command": {"type": "string"}}, "required": ["command"]}},
    {"name": "use_skill", "description": "Load a Skill's full instructions by name (see the Skills list in your context). Call this FIRST when a task matches a skill, then follow what it returns. The skill's folder may contain scripts/files you can read with read_file or run with bash.",
     "input_schema": {"type": "object", "properties": {"name": {"type": "string"}}, "required": ["name"]}},
    {"name": "find_skill", "description": "Search your installed Skills by keyword (matches names + descriptions). Use this when you have many skills and need to find the right one for a task; then call use_skill on the match.",
     "input_schema": {"type": "object", "properties": {"query": {"type": "string"}}, "required": ["query"]}},
]

def tool_find_skill(a):
    q = (a.get("query") or "").lower().strip()
    if not q:
        return "Pass a keyword to search for.", "no query"
    hits = [s for s in _SKILLS.values()
            if q in s["name"].lower() or q in (s["description"] or "").lower()]
    if not hits:
        return "No skills match %r (%d installed). Try a broader keyword." % (q, len(_SKILLS)), "no match"
    rows = []
    for s in hits[:25]:
        d = " ".join((s["description"] or "").split())
        rows.append("- %s: %s" % (s["name"], d[:160]))
    return "Skills matching %r:\n%s" % (q, "\n".join(rows)), "%d match" % len(hits)

def tool_use_skill(a):
    nm = (a.get("name") or "").strip()
    s = _SKILLS.get(nm) or next((v for k, v in _SKILLS.items() if k.lower() == nm.lower()), None)
    if not s:
        return ("No skill named %r. Available skills: %s" % (nm, ", ".join(_SKILLS) or "(none)")), "no such skill"
    body = "# Skill: %s\nfolder: %s\n(read/run files in this folder as the instructions say)\n\n%s" % (
        s["name"], s["dir"], s["body"])
    return body, "skill loaded"

def load_skill_into(history, name):
    """Pull a skill's full instructions into the conversation so the agent applies
    it to whatever the user asks next. Used by /myskills and /<skillname>."""
    s = _SKILLS.get(name) or next((v for k, v in _SKILLS.items() if k.lower() == name.lower()), None)
    if not s:
        print(dim("  that skill isn't installed anymore.")); return
    body, _ = tool_use_skill({"name": s["name"]})
    history.append({"role": "user",
        "content": "Load the \"%s\" skill and apply it to what I ask next.\n\n%s" % (s["name"], body)})
    try: save_session(history)
    except Exception: pass
    print("  " + orange("✦ ") + bold(s["name"]) + dim("  loaded — now tell me what to make."))

def find_vanta():
    for p in [shutil.which("vanta"), os.path.expanduser("~/.vanbrew/bin/vanta")]:
        if p and os.path.exists(p): return p
    return None

MODE = {"v": "default"}   # permission mode: default | auto | plan (shift+tab cycles)
MODE_ORDER = ["default", "auto", "plan"]
# tools that change the world / run code - blocked in plan mode
PLAN_BLOCKED = {"write_file", "edit_file", "make_dir", "move_path", "delete_path",
                "run_vanta", "run_app", "bash"}

def _confirm(action):
    if MODE["v"] == "auto": return True
    ans = ""
    try:
        sys.stdout.write("\n  %s %s " % (orange("Proceed?"), dim("(y / N / a=always)")))
        sys.stdout.flush()
        ans = sys.stdin.readline().strip().lower()
    except (EOFError, KeyboardInterrupt):
        print(); return False
    except Exception:
        ans = ""
    if ans == "a": MODE["v"] = "auto"; return True
    return ans in ("y", "yes")

_DOC_SKILL = {".pdf": "pdf", ".docx": "docx", ".doc": "docx",
              ".xlsx": "xlsx", ".xls": "xlsx", ".pptx": "pptx", ".ppt": "pptx"}
def tool_read_file(a):
    p = os.path.expanduser(a.get("path", ""))
    if not p: return "no path given", "error"
    if not os.path.exists(p):
        return "no such file: %s  (use glob or list_files to find the right path)" % p, "missing"
    if os.path.isdir(p):
        return "%s is a directory, not a file - use list_files or glob to see what's inside." % p, "is a dir"
    ext = os.path.splitext(p)[1].lower()
    skill = _DOC_SKILL.get(ext)
    if skill:                                          # binary doc: don't read as text
        hint = (' Call use_skill("%s") and follow it - the skill bundles scripts to do this.' % skill) \
               if skill in _SKILLS else ' Use the bash tool with a converter (e.g. pdftotext / a python lib).'
        return ("%s is a %s document (binary) - reading it as text won't work. To get its "
                "contents, extract them first.%s" % (os.path.basename(p), ext.lstrip(".").upper(), hint)), "binary doc"
    try:
        with open(p, "r", errors="replace") as f: txt = f.read()
    except Exception as e:
        return ("could not read %s as text (%s). If it's a binary/Office/PDF file, use the "
                "matching skill (use_skill) or the bash tool." % (p, e)), "read error"
    if len(txt) > 60000: txt = txt[:60000] + "\n... (truncated)"
    return txt, "%d lines" % (txt.count("\n") + 1)

def _gutter(c, n, sign):
    label = ("%4d %s " % (n, sign)) if n else ("     %s " % sign)
    return c(label)

def print_diff(old, new):
    new_lines = new.splitlines()
    if not old:                                   # brand-new file: all additions
        print("  " + dim("⎿  ") + green("+%d lines" % len(new_lines)))
        for i, ln in enumerate(new_lines[:26], 1):
            print("     " + _gutter(green, i, "+") + green(ln[:96]))
        if len(new_lines) > 26: print("     " + dim("… +%d more lines" % (len(new_lines) - 26)))
        return
    diff = list(difflib.unified_diff(old.splitlines(), new_lines, n=2, lineterm=""))
    added = sum(1 for l in diff if l[:1] == "+" and not l.startswith("+++"))
    removed = sum(1 for l in diff if l[:1] == "-" and not l.startswith("---"))
    if added == 0 and removed == 0:
        print("  " + dim("⎿  no changes")); return
    print("  " + dim("⎿  ") + green("+%d" % added) + dim("  ") + red("-%d" % removed))
    shown = 0; nl = 0
    for l in diff:
        if l.startswith("+++") or l.startswith("---"): continue
        if l.startswith("@@"):
            m = re.search(r"\+(\d+)", l); nl = int(m.group(1)) if m else nl
            if shown: print("     " + dim("┄┄┄"))
            continue
        if shown >= 44:
            print("     " + dim("… more changes")); break
        if l[:1] == "+":
            print("     " + _gutter(green, nl, "+") + green(l[1:][:96])); nl += 1; shown += 1
        elif l[:1] == "-":
            print("     " + _gutter(red, 0, "-") + red(l[1:][:96])); shown += 1
        else:
            print("     " + _gutter(dim, nl, " ") + dim(l[1:][:96])); nl += 1

def tool_write_file(a):
    p = os.path.expanduser(a["path"]); content = a.get("content", "")
    old = ""
    if os.path.exists(p):
        try: old = open(p).read()
        except Exception: old = ""
    d = os.path.dirname(p)
    if d and not os.path.isdir(d): os.makedirs(d)
    with open(p, "w") as f: f.write(content)
    print_diff(old, content)
    return ("Wrote %s" % p, None)   # None summary: print_diff already drew the ⎿ line

def tool_make_dir(a):
    p = os.path.expanduser(a["path"])
    os.makedirs(p, exist_ok=True)
    return "Created folder %s" % p, "ok"

def tool_move_path(a):
    src = os.path.expanduser(a["from"]); dst = os.path.expanduser(a["to"])
    d = os.path.dirname(dst)
    if d and not os.path.isdir(d): os.makedirs(d)
    shutil.move(src, dst)
    return "Moved %s -> %s" % (src, dst), "ok"

def tool_delete_path(a):
    p = os.path.expanduser(a["path"])
    print("  " + dim("delete " + p))
    if not _confirm("delete"):
        return "User declined to delete this.", "declined"
    if os.path.isdir(p): shutil.rmtree(p)
    elif os.path.exists(p): os.remove(p)
    else: return "nothing at " + p, "missing"
    return "Deleted %s" % p, "ok"

def tool_edit_file(a):
    p = os.path.expanduser(a["path"]); old = a.get("old", ""); new = a.get("new", "")
    if not os.path.exists(p): return "no such file: " + p, "missing"
    content = open(p).read()
    if old == "" or old not in content:
        return "could not find that exact text in %s — copy it verbatim including indentation" % p, "no match"
    cnt = content.count(old)
    if cnt > 1:
        return "that text appears %d times in %s — include more surrounding lines so it is unique" % (cnt, p), "ambiguous"
    updated = content.replace(old, new, 1)
    with open(p, "w") as f: f.write(updated)
    print_diff(content, updated)
    return ("Edited %s" % p, None)

def _py_grep(pat, root):
    try: rx = re.compile(pat)
    except Exception: rx = re.compile(re.escape(pat))
    hits = []
    for dp, dn, fn in os.walk(root):
        dn[:] = [d for d in dn if d not in (".git", "node_modules", "__pycache__", ".vanbrew")]
        for f in fn:
            fp = os.path.join(dp, f)
            try:
                for i, ln in enumerate(open(fp, "r", errors="replace"), 1):
                    if rx.search(ln):
                        hits.append("%s:%d:%s" % (fp, i, ln.rstrip()[:200]))
                        if len(hits) >= 200: return hits
            except Exception:
                continue
    return hits

def tool_search(a):
    pat = a["pattern"]; root = os.path.expanduser(a.get("path", "."))
    rg = shutil.which("rg")
    if rg:
        try:
            r = subprocess.run([rg, "-n", "--no-heading", "-S", "-m", "200", pat, root],
                               stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, timeout=20)
            lines = r.stdout.decode("utf-8", "replace").splitlines()
        except Exception:
            lines = _py_grep(pat, root)
    else:
        lines = _py_grep(pat, root)
    if not lines: return "no matches for %r" % pat, "0 matches"
    body = "\n".join(lines[:80])
    if len(lines) > 80: body += "\n… %d more matches" % (len(lines) - 80)
    return body, "%d matches" % len(lines)

def tool_glob(a):
    import fnmatch
    pat = a["pattern"]; root = os.path.expanduser(a.get("path", "."))
    out = []
    for dp, dn, fn in os.walk(root):
        dn[:] = [d for d in dn if d not in (".git", "node_modules", "__pycache__", ".vanbrew")]
        for f in fn:
            if fnmatch.fnmatch(f, pat):
                out.append(os.path.join(dp, f))
        if len(out) >= 300: break
    return ("\n".join(out) if out else "no files match %r" % pat), "%d files" % len(out)

def tool_list_files(a):
    p = os.path.expanduser(a.get("path", "."))
    items = sorted(os.listdir(p))
    out = "\n".join(("%s/" % i if os.path.isdir(os.path.join(p, i)) else i) for i in items)
    return out or "(empty)", "%d items" % len(items)

def tool_run_vanta(a):
    p = os.path.expanduser(a["path"]); v = find_vanta()
    if not v: return "vanta CLI not found. Install it with: vanbrew install vanta", "no vanta"
    try:
        r = subprocess.run([v, p], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=30)
        out = r.stdout.decode("utf-8", "replace")
        return out or "(no output)", "exit %d" % r.returncode
    except subprocess.TimeoutExpired:
        return "(timed out after 30s - likely a serve()/loop program; run it yourself: vanta %s)" % p, "timeout"

def tool_bash(a):
    cmd = a["command"]
    print("  " + dim("$ " + cmd))
    if not _confirm("bash"):
        return "User declined to run this command.", "declined"
    try:
        r = subprocess.run(cmd, shell=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=60)
        out = r.stdout.decode("utf-8", "replace")
        if len(out) > 20000: out = out[:20000] + "\n... (truncated)"
        return out or "(no output)", "exit %d" % r.returncode
    except subprocess.TimeoutExpired:
        return "(timed out after 60s)", "timeout"

def find_chrome():
    for p in ["/Applications/Google Chrome.app/Contents/MacOS/Google Chrome",
              shutil.which("google-chrome"), shutil.which("chromium"), shutil.which("chrome")]:
        if p and os.path.exists(p): return p
    return None

def _child_port(pid):
    # the TCP port THIS process is actually listening on (so we never open a
    # port some OTHER app already owns, e.g. a conlang already on 8080)
    try:
        out = subprocess.run(["lsof", "-nP", "-iTCP", "-sTCP:LISTEN", "-a", "-p", str(pid)],
                             stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, timeout=4).stdout.decode("utf-8", "replace")
        m = re.search(r":(\d+)\s*\(LISTEN\)", out)
        return m.group(1) if m else None
    except Exception:
        return None

def tool_run_app(a):
    path = os.path.expanduser(a["path"])
    if not os.path.exists(path): return "no such file: " + path, "missing"
    v = find_vanta()
    if not v: return "vanta CLI not found — run: vanbrew install vanta", "no vanta"
    try: src = open(path).read()
    except Exception as e: return "could not read %s: %s" % (path, e), "error"
    chrome = find_chrome()
    if "serve(" in src:   # a web server: launch it, open ITS port in a movable window
        proc = subprocess.Popen([v, path], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        port = None
        for _ in range(14):                 # wait up to ~5s for it to bind
            time.sleep(0.35)
            port = _child_port(proc.pid)
            if port: break
            if proc.poll() is not None: break  # it exited (likely a port clash)
        if not port:
            m = re.search(r"serve\(\s*(\d{2,5})", src) or re.search(r"PORT\s+be\s+(\d{2,5})", src)
            sp = m.group(1) if m else None
            if sp and sp in ("8080", "8090", "8100"):
                return ("%s did not start — port %s is already taken by another app (the user runs one there). Rewrite it to serve on a free high port like 8765, then run_app again." % (os.path.basename(path), sp)), "port busy"
            return ("%s did not start a server (no listening port). Check it serves on a free port and try again." % os.path.basename(path)), "no port"
        url = "http://localhost:%s/" % port
        if chrome:
            subprocess.Popen([chrome, "--app=" + url, "--window-size=980,720"],
                             stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            return "Launched %s — serving %s in a movable window." % (os.path.basename(path), url), "window " + url
        return "Launched %s — serving %s (open it in Chrome)." % (os.path.basename(path), url), url
    # otherwise it writes a page and opens it: force a movable chromeless app-window
    env = dict(os.environ)
    if chrome: env["BROWSER"] = '"%s" --app=%%s' % chrome
    try:
        r = subprocess.run([v, path], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=25, env=env)
        out = r.stdout.decode("utf-8", "replace")
        return (out or "(ran — its window should pop up)"), "window opened"
    except subprocess.TimeoutExpired:
        return "(still running after 25s — if it serves, it's up; open it in Chrome)", "running"

DISPATCH = {"read_file": tool_read_file, "write_file": tool_write_file,
            "edit_file": tool_edit_file, "search": tool_search, "glob": tool_glob,
            "list_files": tool_list_files, "make_dir": tool_make_dir,
            "move_path": tool_move_path, "delete_path": tool_delete_path,
            "run_vanta": tool_run_vanta, "run_app": tool_run_app, "bash": tool_bash,
            "use_skill": tool_use_skill, "find_skill": tool_find_skill}

def tool_label(name, a):
    if name == "read_file":  return "Read(%s)" % a.get("path", "")
    if name == "write_file":
        return ("Update(%s)" if os.path.exists(os.path.expanduser(a.get("path", ""))) else "Write(%s)") % a.get("path", "")
    if name == "edit_file":  return "Edit(%s)" % a.get("path", "")
    if name == "search":     return "Search(%s)" % a.get("pattern", "")
    if name == "glob":       return "Glob(%s)" % a.get("pattern", "")
    if name == "list_files": return "List(%s)" % a.get("path", ".")
    if name == "make_dir":   return "Mkdir(%s)" % a.get("path", "")
    if name == "move_path":  return "Move(%s -> %s)" % (a.get("from", ""), a.get("to", ""))
    if name == "delete_path":return "Delete(%s)" % a.get("path", "")
    if name == "run_vanta":  return "Run(%s)" % a.get("path", "")
    if name == "run_app":    return "Open app(%s)" % a.get("path", "")
    if name == "bash":       return "Bash(%s)" % a.get("command", "")[:50]
    return "%s(%s)" % (name, a)

def run_tool(name, a):
    SESSION["tools"] += 1
    if isinstance(a, dict) and "_truncated" in a:
        print(orange("⏺ ") + bold(name) + dim("  · arguments were cut off (output limit)"))
        return ("[Your last tool call was CUT OFF at the output limit, so its arguments "
                "are incomplete - it was NOT run. Don't re-type a whole large file: instead "
                "use bash `cp` to copy the original, then edit_file for the small changes.]")
    if MODE["v"] == "plan" and name in PLAN_BLOCKED:
        print(orange("⏺ ") + bold(tool_label(name, a)) + dim("  · plan mode, skipped"))
        return ("[PLAN MODE is on - read-only. Do not write, edit, or run anything. "
                "Keep exploring with read_file/search/glob/list_files only, then STOP and "
                "present a short, concrete PLAN of the steps you would take. The user will "
                "press shift+tab to leave plan mode and tell you to go.]")
    print(orange("⏺ ") + bold(tool_label(name, a)))
    try:
        result, summary = DISPATCH[name](a)
    except Exception as e:
        result, summary = ("Error: %s" % e), "error"
    if summary is not None:          # tools that drew their own ⎿ (diffs) return None
        print("  " + dim("⎿  " + summary))
    return result

# --------------------------------------------------------------- LLM client --
def http_json(url, payload, headers, timeout=300, retries=3):
    """POST JSON and parse the reply. Big generations can take minutes, so the read
    timeout is generous (300s) and transient failures - read/connect timeouts, dropped
    connections, 429/5xx - are retried with backoff instead of killing the turn."""
    import socket
    data = json.dumps(payload).encode("utf-8")
    last = None
    for attempt in range(retries):
        req = urllib.request.Request(url, data=data, headers=headers, method="POST")
        try:
            with urllib.request.urlopen(req, timeout=timeout) as r:
                return json.loads(r.read().decode("utf-8", "replace"))
        except urllib.error.HTTPError as e:
            body = e.read().decode("utf-8", "replace")
            if e.code in (408, 409, 425, 429, 500, 502, 503, 504) and attempt < retries - 1:
                last = RuntimeError("API error %s: %s" % (e.code, body[:300]))
                time.sleep(1.5 * (attempt + 1)); continue
            raise RuntimeError("API error %s: %s" % (e.code, body[:500]))
        except (socket.timeout, TimeoutError):
            last = RuntimeError("the API kept timing out (no response within %ds). "
                                "The model may be overloaded — try again, or pick a faster "
                                "model with /model." % timeout)
            if attempt < 1: time.sleep(1.0); continue      # one extra try only (don't burn N×timeout)
            raise last
        except urllib.error.URLError as e:
            reason = getattr(e, "reason", e)
            last = RuntimeError("could not reach the API: %s" % reason)
            if attempt < retries - 1: time.sleep(1.5 * (attempt + 1)); continue
            raise last
        except (ConnectionError, socket.error) as e:           # reset/broken pipe mid-stream
            last = RuntimeError("connection dropped: %s" % e)
            if attempt < retries - 1: time.sleep(1.5 * (attempt + 1)); continue
            raise last
    if last: raise last

def to_openai_msgs(history):
    out = []
    for m in history:
        if isinstance(m["content"], str):
            out.append({"role": m["role"], "content": m["content"]}); continue
        if m["role"] == "assistant":
            text = ""; calls = []
            for b in m["content"]:
                if b["type"] == "text": text += b["text"]
                elif b["type"] == "tool_use":
                    calls.append({"id": b["id"], "type": "function",
                                  "function": {"name": b["name"], "arguments": json.dumps(b["input"])}})
            msg = {"role": "assistant", "content": text}   # "" not None: Ollama/OpenAI reject null content
            if calls: msg["tool_calls"] = calls
            out.append(msg)
        else:  # user with tool_result blocks
            for b in m["content"]:
                if b["type"] == "tool_result":
                    c = b.get("content", "")
                    out.append({"role": "tool", "tool_call_id": b["tool_use_id"], "content": c if isinstance(c, str) else str(c)})
                elif b["type"] == "text":
                    out.append({"role": "user", "content": b["text"]})
    return out

def call_llm(cfg, history):
    """Return a normalized assistant message: {content:[blocks], stop:'tool'|'end'}."""
    if cfg["kind"] == "anthropic":
        payload = {"model": cfg["model"], "max_tokens": MAX_TOKENS, "system": SYSTEM + _CTX["text"],
                   "messages": history, "tools": TOOLS}
        headers = {"x-api-key": cfg["key"], "anthropic-version": "2023-06-01",
                   "content-type": "application/json"}
        j = http_json("https://api.anthropic.com/v1/messages", payload, headers)
        u = j.get("usage", {}) or {}
        USAGE["ctx"] = u.get("input_tokens", USAGE["ctx"]); USAGE["out"] += u.get("output_tokens", 0)
        blocks = j.get("content", [])
        stop = "tool" if j.get("stop_reason") == "tool_use" else "end"
        return {"content": blocks, "stop": stop, "trunc": j.get("stop_reason") == "max_tokens"}
    else:  # openai-compatible (openrouter)
        oai_tools = [{"type": "function", "function": {"name": t["name"], "description": t["description"],
                      "parameters": t["input_schema"]}} for t in TOOLS]
        msgs = [{"role": "system", "content": SYSTEM + _CTX["text"]}] + to_openai_msgs(history)
        payload = {"model": cfg["model"], "max_tokens": MAX_TOKENS, "messages": msgs,
                   "tools": oai_tools, "tool_choice": "auto"}
        headers = {"Authorization": "Bearer " + cfg["key"], "content-type": "application/json",
                   "HTTP-Referer": "https://github.com/Juanshep1/vanbrew", "X-Title": "Vanta Code"}
        j = http_json(cfg["base"] + "/chat/completions", payload, headers)
        u = j.get("usage", {}) or {}
        USAGE["ctx"] = u.get("prompt_tokens", USAGE["ctx"]); USAGE["out"] += u.get("completion_tokens", 0)
        ch = j["choices"][0]; m = ch["message"]
        trunc = ch.get("finish_reason") == "length"
        blocks = []
        if m.get("content"): blocks.append({"type": "text", "text": m["content"]})
        for tc in (m.get("tool_calls") or []):
            raw = tc["function"]["arguments"] or "{}"
            try:
                inp = json.loads(raw)
            except Exception:
                inp = {"_truncated": raw}    # cut-off JSON args -> flag it, don't run silently empty
            blocks.append({"type": "tool_use", "id": tc["id"], "name": tc["function"]["name"], "input": inp})
        stop = "tool" if (m.get("tool_calls")) else "end"
        return {"content": blocks, "stop": stop, "trunc": trunc}

# ----------------------------------------------------------------- agent -----
def wrap(text, width):
    out = []
    for para in text.split("\n"):
        if not para: out.append(""); continue
        line = ""
        for word in para.split(" "):
            if line and len(line) + 1 + len(word) > width:
                out.append(line); line = word
            else:
                line = (line + " " + word) if line else word
        out.append(line)
    return "\n".join(out)

def _md_inline(s):
    if not COLOR: return s
    s = re.sub(r"`([^`]+)`", lambda m: orange(m.group(1)), s)        # `code`
    s = re.sub(r"\*\*([^*]+)\*\*", lambda m: bold(m.group(1)), s)     # **bold**
    return s

def print_assistant(text):
    rendered = []
    fence = False
    for raw in text.strip().split("\n"):
        st = raw.strip()
        if st.startswith("```"):
            fence = not fence
            rendered.append(dim("  " + "┄" * 24)); continue
        if fence:
            rendered.append(_hl_code(raw)); continue
        h = re.match(r"^(#{1,6})\s+(.*)", raw)
        if h: rendered.append(bold(orange(h.group(2)))); continue
        b = re.match(r"^(\s*)[-*]\s+(.*)", raw)
        if b: rendered.append(b.group(1) + orange("•") + " " + _md_inline(b.group(2))); continue
        rendered.append(_md_inline(raw))
    first = True
    for ln in rendered:
        if first and ln.strip():
            print(orange("⏺ ") + ln); first = False
        else:
            print("  " + ln)

COMPACT_AT = 60000   # auto-summarise the history once it grows past ~this many chars

def history_size(history):
    try: return sum(len(json.dumps(m.get("content", ""))) for m in history)
    except Exception: return 0

def _transcript(history):
    lines = []
    for m in history:
        c = m.get("content")
        if isinstance(c, str):
            lines.append("%s: %s" % (m["role"], c)); continue
        for b in (c or []):
            t = b.get("type")
            if t == "text": lines.append("%s: %s" % (m["role"], b.get("text", "")))
            elif t == "tool_use": lines.append("assistant -> %s(%s)" % (b.get("name"), json.dumps(b.get("input", {}))[:200]))
            elif t == "tool_result": lines.append("tool result: %s" % (str(b.get("content", ""))[:300]))
    return "\n".join(lines)

def compact_history(cfg, history):
    if not history: return history
    ask = [{"role": "user", "content":
            "Summarize this coding session concisely for your own future reference. "
            "Preserve: what the user is building, exact file paths you created/edited, key decisions, "
            "the current state, and anything still unresolved. A few tight bullet points.\n\n"
            "=== TRANSCRIPT ===\n" + _transcript(history)}]
    sys.stdout.write("  " + dim("compacting the conversation to save context…")); sys.stdout.flush()
    try:
        resp = call_llm(cfg, ask)
        text = "".join(b.get("text", "") for b in resp["content"] if b.get("type") == "text").strip()
    except Exception:
        sys.stdout.write("\r\033[K"); return history
    sys.stdout.write("\r\033[K")
    if not text: return history
    print("  " + dim("↻ compacted earlier conversation to save context"))
    return [{"role": "user", "content": "[Summary of our earlier conversation]\n" + text}]

def _call_worker(cfg, history, box):
    try: box["r"] = call_llm(cfg, history)
    except Exception as e: box["e"] = e

def _finalize_interrupt(history):
    # after a Ctrl-C, leave history in a valid (alternating, tool-results-present) state
    if not history: return
    last = history[-1]
    if last.get("role") == "assistant":
        content = last.get("content")
        if isinstance(content, list):
            ids = [b.get("id") for b in content
                   if isinstance(b, dict) and b.get("type") == "tool_use"]
            if ids:   # tool calls left without results -> satisfy them
                history.append({"role": "user", "content":
                    [{"type": "tool_result", "tool_use_id": i, "content": "(interrupted)"} for i in ids]})
        return
    history.append({"role": "assistant", "content": [{"type": "text", "text": "(interrupted)"}]})

def agent_turn(cfg, history, user_text):
    if history_size(history) > COMPACT_AT:        # auto-compact long sessions
        history[:] = compact_history(cfg, history)
    history.append({"role": "user", "content": user_text})
    SESSION["turns"] += 1
    # The LLM call runs in a worker thread while the main thread animates the
    # spinner and stays responsive to Ctrl-C (a signal — reliable regardless of
    # terminal mode or threads, unlike the old raw-stdin Esc watcher).
    try:
        for _ in range(60):
            sp = Spinner(SPIN_WORDS[int(time.time()) % len(SPIN_WORDS)]).start()
            box = {}
            th = threading.Thread(target=_call_worker, args=(cfg, history, box)); th.daemon = True; th.start()
            try:
                while th.is_alive(): th.join(0.1)
            finally:
                sp.stop()
            if "e" in box:
                print(red("⏺ " + str(box["e"]))); return
            resp = box.get("r")
            if not resp: return
            history.append({"role": "assistant", "content": resp["content"]})
            results = []
            for b in resp["content"]:
                if b["type"] == "text" and b["text"].strip():
                    print_assistant(b["text"])
                elif b["type"] == "tool_use":
                    out = run_tool(b["name"], b.get("input", {}))
                    results.append({"type": "tool_result", "tool_use_id": b["id"], "content": out})
            tool_uses = any(b["type"] == "tool_use" for b in resp["content"])
            if tool_uses: history.append({"role": "user", "content": results})
            if resp.get("trunc"):                  # cut off at the output limit
                print(dim("  ⚠ hit the output limit — recovering automatically"))
                if not tool_uses:
                    history.append({"role": "user", "content":
                        "Your reply was cut off at the length limit. Continue from exactly where you stopped."})
                continue                           # loop again so it finishes the work itself
            if resp["stop"] != "tool" or not tool_uses:
                return
        print(dim("  (stopped after too many steps)"))
    except KeyboardInterrupt:
        _finalize_interrupt(history)
        print(dim("\n  ⎚ interrupted"))

# ----------------------------------------------------------------- ui --------
def box(lines, width=None):
    w = width or term_width()
    inner = w - 4
    top = "╭" + "─" * (w - 2) + "╮"
    bot = "╰" + "─" * (w - 2) + "╯"
    print(orange(top))
    for ln in lines:
        # ln may contain colour codes; pad on visible length is approximate
        print(orange("│ ") + ln)
    print(orange(bot))

def banner(cfg):
    w = max(len(l) for l in VANTA_ART)
    print()
    for line in VANTA_ART:
        print("  " + grad_line(line))
    print("  " + orange("c o d e") + dim("   ·   the terminal agent that speaks Vanta   ·   v" + VERSION))
    print("  " + dim("─" * w))
    dot = green("●") if cfg.get("key") else grey("○")
    print("  " + dot + "  " + bold(cfg["provider"]) + dim("   ·   ") + cfg["model"])
    print("  " + dim(os.getcwd()))
    print()
    print(dim("  type / and Tab to autocomplete a command   ·   ask me to build something   ·   Ctrl-D to exit"))
    print()

HELP = """  Commands:
    /help            show this help
    /clear           start a fresh conversation
    /compact         summarize the conversation now (also happens automatically)
    /resume          reload your previous session (or start with: vcode --continue)
    /init            scan the project and write a VANTA.md (auto-loaded next time)
    /skills          browse Skills; highlight one + Enter pins it to My Skills (★)
    /skills install  grab a pile of skills from a repo (default: Anthropic's official skills)
    /myskills        your pinned skills; Enter loads one to use (also: /<skill-name>)
    /skills remove <name>  unpin a skill from My Skills
    /themes          pick a color theme (ember, synthwave, matrix, ice, gold, mono)
    /provider [name] list providers, or switch: anthropic | openrouter | ollama
    /key             paste/replace the API key for the current provider
    /model [n|name]  list models and pick one (/model 2), or set any id
    /mode            cycle permission mode (handy on phones without Shift+Tab)
    /auto            toggle auto-accept mode (runs everything without asking)
    /plan            toggle plan mode (read-only; the agent proposes a plan first)
    /cost            show session time, turns, tool calls and token usage
    /cwd <path>      change working directory
    /exit, /quit     leave

  Permission modes (current: %s)  -  press Shift+Tab to cycle:
    default          writes auto-approved; shell & delete ask first
    auto-accept      everything runs without asking
    plan             read-only: I explore and propose a plan, then you run it

  Shortcuts:
    /                press / to pop the command menu (↑/↓ to pick, type to filter)
    Shift+Tab        cycle permission mode (default -> auto -> plan)
    Ctrl-C           interrupt the agent while it's working (back to the prompt)
    /                type / and a command autosuggests (blue) - Tab fills it in
    Tab              accept the inline suggestion (/command, #skill, or @file)
    \"\"\"              start a multi-line message (end with \"\"\" on its own line)
    !<command>       run a shell command directly (e.g. !ls, !git status)
    @path/to/file    inline a file's contents into your message
    #skill-name      reference a Skill right in your prompt (e.g. "make a #pdf invoice")
    ↑ / ↓            recall previous prompts (history is saved)

  Just type what you want, e.g.:
    "write a fizzbuzz in vanta and run it"
    "make a web app that serves a todo list on port 8123"
    "read snake.va and explain what it does"
"""

def _is_libedit():
    try:
        import readline
        return "libedit" in (readline.__doc__ or "")
    except Exception:
        return False

def _rl_prompt():
    # The prompt's reported width must equal its VISIBLE width, or readline mis-wraps
    # long input (the line redraws over itself - "an erasable line"). GNU readline
    # honors \001..\002 around non-printing bytes, so we can colour it. macOS libedit
    # does NOT - it counts the escape bytes as width and splits them on wrap - so there
    # we use a PLAIN prompt (multibyte glyphs are fine; only escapes break it). Plain
    # also keeps Tab-completion working under libedit.
    if COLOR and not _is_libedit():
        code = "\033[%sm" % _code(THEME["accent"])
        return "\001%s\002│ › \001\033[0m\002" % code
    return "│ › "

SLASH_COMMANDS = [
    ("/help",     "show the help"),
    ("/clear",    "start a fresh conversation"),
    ("/compact",  "summarize the conversation now"),
    ("/cost",     "session time · turns · tokens"),
    ("/resume",   "reload your previous session"),
    ("/init",     "scan the project, write VANTA.md"),
    ("/skills",   "browse skills · Enter pins one to My Skills"),
    ("/myskills", "your pinned skills · Enter loads one to use"),
    ("/themes",   "pick a colour theme"),
    ("/provider", "switch AI provider"),
    ("/key",      "set the API key"),
    ("/model",    "pick the model"),
    ("/mode",     "cycle permission mode (default/auto/plan)"),
    ("/auto",     "toggle auto-accept mode"),
    ("/plan",     "toggle plan mode (read-only)"),
    ("/cwd",      "change working directory"),
    ("/exit",     "quit"),
]

_HIST = []   # the prompt history, newest last

def _load_history():
    try:
        with open(os.path.expanduser("~/.vanta-code/history")) as f:
            _HIST[:] = [ln.rstrip("\n") for ln in f if ln.strip()]
    except Exception:
        pass

def _save_history_line(line):
    if not line or (_HIST and _HIST[-1] == line):
        return
    _HIST.append(line)
    p = os.path.expanduser("~/.vanta-code/history")
    try:
        os.makedirs(os.path.dirname(p), exist_ok=True)
        with open(p, "a") as f:
            f.write(line + "\n")
    except Exception:
        pass

def _editor_prompt():
    # (display string, visible width). We position the cursor by column ourselves,
    # so colour escapes are fine even on libedit - their bytes don't move the cursor.
    m = MODE["v"]
    if m == "auto":
        return ("\033[%sm│ auto › \033[0m" % GREEN if COLOR else "auto › "), (9 if COLOR else 7)
    if m == "plan":
        return ("\033[%sm│ plan › \033[0m" % BLUE if COLOR else "plan › "), (9 if COLOR else 7)
    if COLOR:
        return "\033[%sm│ › \033[0m" % _code(THEME["accent"]), 4
    return "│ › ", 4

def _mode_banner():
    # glyphs kept ASCII-safe (»/•) so they render on Android terminal fonts too
    m = MODE["v"]
    if m == "auto":
        print("  " + green("» auto-accept on") + dim("  · runs everything without asking  · shift+tab or /mode"))
    elif m == "plan":
        print("  " + blue("• plan mode on") + dim("  · read-only, I'll propose a plan first  · shift+tab or /mode"))

def _slash_menu():
    rows = ["%-12s %s" % (c, dim(d)) for c, d in SLASH_COMMANDS]
    cmds = [c for c, _ in SLASH_COMMANDS]
    for n in my_skill_names():                         # your pinned skills, typeable as /<name>
        rows.append("%-12s %s" % ("/" + n, orange("★ ") + dim("my skill — load & use")))
        cmds.append("/" + n)
    sel = select_menu(orange("  / ") + dim("pick a command   ↑/↓ Enter · Esc to cancel"), rows)
    return cmds[sel] if sel is not None else None

def _complete_word(word):
    import glob as _glob
    if word.startswith("#"):                            # #skill-name -> complete skill names
        pref = word[1:].lower()
        ms = [n for n in _SKILLS if n.lower().startswith(pref)]
        if not ms: return None
        return "#" + (os.path.commonprefix(ms) if len(ms) > 1 else ms[0])
    at = word.startswith("@")
    pref = word[1:] if at else word
    if not pref:
        return None
    try:
        ms = sorted(_glob.glob(os.path.expanduser(pref) + "*"))[:50]
    except Exception:
        return None
    if not ms:
        return None
    ms = [m + ("/" if os.path.isdir(m) else "") for m in ms]
    cp = os.path.commonprefix(ms)
    return ("@" if at else "") + cp

def _suggest_token(s, cur):
    """Inline autosuggestion for the word ending at the cursor. Returns
    (word_start, word, full_completion) for a /command, a /<skill>, or a
    #skill-name - the first match in natural order - or None."""
    j = cur
    while j > 0 and s[j - 1] not in (" ", "\t"): j -= 1
    word = s[j:cur]
    if not word: return None
    if word[0] == "/":
        pinned = my_skill_names()                       # pinned skills rank first,
        rest = [n for n in _SKILLS if n not in pinned]  # then every other installed skill
        cands = [c for c, _ in SLASH_COMMANDS] + ["/" + n for n in pinned + rest]
    elif word[0] == "#":
        cands = ["#" + n for n in _SKILLS]
    else:
        return None
    wl = word.lower()
    for c in cands:
        if c.lower().startswith(wl) and len(c) > len(word):
            return (j, word, c)
    return None

def _decorate_line(s):
    # colour a leading /command (or /<skill>) token blue, so a slash command stands out
    if not s or s[0] != "/": return s
    i = 0
    while i < len(s) and s[i] not in (" ", "\t"): i += 1
    return blue(s[:i]) + s[i:]

def read_line():
    """A small raw-mode line editor. Lone '/' on an empty line opens a command
    dropdown (↑/↓ to browse, Enter/Tab to pick); start typing and it collapses to
    an inline autosuggest (blue command + faint ghost) you accept with Tab. Long
    input WORD-WRAPS onto aligned continuation rows (no horizontal scroll). Shift+Tab
    cycles the permission mode; ↑/↓ recall history; ←/→ move; batched input (paste,
    held keys, phones) is handled char-by-char. Falls back to input() (no tty)."""
    if not (sys.stdin.isatty() and sys.stdout.isatty()):
        return input(_editor_prompt()[0])
    try:
        import termios
    except Exception:
        return input(_editor_prompt()[0])
    fd = sys.stdin.fileno()
    try:
        old = termios.tcgetattr(fd)
    except Exception:
        return input(_editor_prompt()[0])
    buf = []; cur = [0]; hidx = [len(_HIST)]; saved = [""]
    drawn_rows = [1]; cur_row = [0]                  # last render's row count + cursor row
    def _layout(text, fw, cw):
        # word-wrap into visual rows: list of (start, end) char ranges. Breaks at the
        # last space that fits; falls back to a hard break for an over-long word.
        rows = []; i = 0; n = len(text); width = fw
        while True:
            if n - i <= width:
                rows.append((i, n)); break
            seg = text[i:i + width]; sp = seg.rfind(" ")
            brk = i + sp + 1 if sp > 0 else i + width
            rows.append((i, brk)); i = brk; width = cw
        return rows
    def render(final=False):
        prompt_disp, prompt_w = _editor_prompt()    # recompute so mode changes show live
        W = max(20, term_width()); usable = W - 1
        fw = max(4, usable - prompt_w); indent = prompt_w; cw = max(4, usable - indent)
        full = "".join(buf)
        rows = _layout(full, fw, cw)
        crow = len(rows) - 1; ccol = cur[0] - rows[-1][0]    # cursor row/col
        for ri, (a, bnd) in enumerate(rows):
            if a <= cur[0] < bnd: crow = ri; ccol = cur[0] - a; break
        if cur_row[0] > 0: sys.stdout.write("\033[%dA" % cur_row[0])   # to row 0 of last draw
        sys.stdout.write("\r\033[J")                                   # clear it + everything below
        lines = []
        for ri, (a, bnd) in enumerate(rows):
            seg = full[a:bnd]
            if ri == 0:
                disp = seg
                if COLOR and len(rows) == 1:          # single line: blue /command + faint ghost
                    disp = _decorate_line(seg)
                    if cur[0] == len(buf) and not final:
                        sg = _suggest_token(full, cur[0])
                        if sg:
                            suf = sg[2][len(sg[1]):]
                            if suf and prompt_w + len(seg) + len(suf) <= usable:
                                disp += dim(suf)
                lines.append(prompt_disp + disp)
            else:
                lines.append((" " * indent) + seg)
        sys.stdout.write("\n".join(lines))
        up = (len(rows) - 1) - crow                   # from last drawn row back to cursor row
        if up > 0: sys.stdout.write("\033[%dA" % up)
        sys.stdout.write("\r")
        tcol = (prompt_w if crow == 0 else indent) + ccol
        if tcol > 0: sys.stdout.write("\033[%dC" % tcol)
        sys.stdout.flush()
        drawn_rows[0] = len(rows); cur_row[0] = crow
    # an inline command dropdown: lone '/' opens it (browse with ↑/↓, Enter/Tab to
    # pick); typing any letter collapses it back to the inline ghost-fill editor.
    menu = [False]; msel = [0]; mtop = [0]; mitems = [[]]
    def menu_draw():
        prompt_disp, prompt_w = _editor_prompt()
        items = mitems[0]; m = len(items)
        MV = min(m, 8)
        if msel[0] < mtop[0]: mtop[0] = msel[0]
        elif msel[0] >= mtop[0] + MV: mtop[0] = msel[0] - MV + 1
        if mtop[0] > max(0, m - MV): mtop[0] = max(0, m - MV)
        if mtop[0] < 0: mtop[0] = 0
        descw = max(4, term_width() - 16)
        out = ["\r\033[K" + prompt_disp + blue("/")]
        for r in range(MV):
            mi = mtop[0] + r
            cmd, desc = items[mi]; desc = desc[:descw]
            if mi == msel[0]:
                out.append("\r\033[K" + orange("❯ ") + bold(cmd.ljust(12)) + " " + desc)
            else:
                out.append("\r\033[K  " + blue(cmd.ljust(12)) + " " + dim(desc))
        sys.stdout.write("\n".join(out))
        sys.stdout.write("\033[%dA\r\033[%dC" % (MV, prompt_w + 1))   # back to input line, after '/'
        sys.stdout.flush()
    def menu_erase():
        sys.stdout.write("\r\033[J"); sys.stdout.flush()   # clear input line + dropdown below
    def menu_open():                                   # pinned skills first, then commands
        mitems[0] = [("/" + n, "★ my skill") for n in my_skill_names()] + list(SLASH_COMMANDS)
        menu[0] = True; msel[0] = 0; mtop[0] = 0
        buf[:] = ["/"]; cur[0] = 1
        menu_draw()
    def raw():
        nw = termios.tcgetattr(fd)
        nw[3] = nw[3] & ~(termios.ICANON | termios.ECHO | termios.ISIG)
        termios.tcsetattr(fd, termios.TCSANOW, nw)
    import select as _select, errno as _errno
    try:
        raw(); render()
        while True:
            try:
                b = os.read(fd, 64)
            except OSError as e:                               # e.g. EINTR when the
                if e.errno == _errno.EINTR: continue           # app is backgrounded
                raise
            if not b:
                continue
            if b == b"\x1b":                                   # lone Esc: a phone/laggy
                r, _, _ = _select.select([fd], [], [], 0.05)   # terminal may split the
                if r:                                          # arrow escape across reads
                    try: b += os.read(fd, 8)
                    except OSError: pass
            if menu[0]:                                        # the '/' dropdown owns keys
                if b[:1] == b"\x1b" and b[1:2] == b"[":         # ↑/↓ browse (batch-safe)
                    up = b.count(b"\x1b[A"); down = b.count(b"\x1b[B")
                    if up or down:
                        msel[0] = (msel[0] + down - up) % len(mitems[0]); menu_draw()
                    continue
                if b == b"\x1b":                               # Esc cancels the dropdown
                    menu[0] = False; menu_erase(); buf[:] = []; cur[0] = 0; render(); continue
                bs = bytearray(b); i = 0
                while i < len(bs):
                    byte = bs[i]; i += 1
                    if byte in (9, 10, 13):                    # Tab / Enter -> pick the highlighted
                        cmd = mitems[0][msel[0]][0]
                        menu[0] = False; menu_erase()
                        pd, _pw = _editor_prompt()
                        sys.stdout.write(pd + _decorate_line(cmd) + "\n"); sys.stdout.flush()
                        termios.tcsetattr(fd, termios.TCSADRAIN, old)
                        return cmd
                    if byte == 3:                              # Ctrl-C
                        termios.tcsetattr(fd, termios.TCSADRAIN, old); raise KeyboardInterrupt
                    if byte in (8, 127):                       # backspace deletes '/', closes
                        menu[0] = False; menu_erase(); buf[:] = []; cur[0] = 0; render(); break
                    if 32 <= byte <= 126:                      # typing -> collapse to inline fill
                        menu[0] = False; menu_erase()
                        buf[:] = ["/", chr(byte)]; cur[0] = 2
                        while i < len(bs):                     # carry any further typed chars
                            nb = bs[i]; i += 1
                            if 32 <= nb <= 126: buf.insert(cur[0], chr(nb)); cur[0] += 1
                        render(); break
                continue
            if b[:1] == b"\x1b":                               # escape / arrows (whole read)
                if b[1:2] == b"[":
                    z = b.count(b"\x1b[Z")                     # Shift+Tab cycles the mode
                    if z:
                        for _ in range(z):
                            MODE["v"] = MODE_ORDER[(MODE_ORDER.index(MODE["v"]) + 1) % len(MODE_ORDER)]
                        render(); continue
                    right = b.count(b"\x1b[C"); left = b.count(b"\x1b[D")   # count: held key batches
                    if right or left:
                        cur[0] = max(0, min(len(buf), cur[0] + right - left)); render(); continue
                    if b"\x1b[A" in b and hidx[0] > 0:         # Up: previous history
                        if hidx[0] == len(_HIST): saved[0] = "".join(buf)
                        hidx[0] -= 1; buf[:] = list(_HIST[hidx[0]]); cur[0] = len(buf); render(); continue
                    if b"\x1b[B" in b and hidx[0] < len(_HIST):  # Down: next history
                        hidx[0] += 1
                        buf[:] = list(_HIST[hidx[0]]) if hidx[0] < len(_HIST) else list(saved[0])
                        cur[0] = len(buf); render(); continue
                    if b"\x1b[H" in b: cur[0] = 0; render(); continue
                    if b"\x1b[F" in b: cur[0] = len(buf); render(); continue
                continue
            if b == b"\t":                                     # Tab: accept the suggestion
                s = "".join(buf); j = cur[0]
                while j > 0 and s[j - 1] not in (" ", "\t"):
                    j -= 1
                word = s[j:cur[0]]
                if word and word[0] in "/#":                   # fill in the /command or #skill
                    sg = _suggest_token(s, cur[0])
                    if sg:
                        buf[j:cur[0]] = list(sg[2]); cur[0] = j + len(sg[2]); render()
                    continue
                comp = _complete_word(word) if word else None  # @file / path completion
                if comp and comp != word:
                    buf[j:cur[0]] = list(comp); cur[0] = j + len(comp); render()
                continue
            if b == b"/" and not buf:                          # lone '/' opens the dropdown
                menu_open(); continue
            # everything else: process the read CHARACTER-BY-CHARACTER so a batched read
            # (paste, key-repeat, phone keyboards) of text + backspaces + Enter is safe.
            try: s = b.decode("utf-8")
            except Exception:
                try: s = b.decode("utf-8", "ignore")
                except Exception: s = ""
            submit = False
            for ch in s:
                o = ord(ch)
                if o in (10, 13): submit = True; break          # Enter
                if o == 3:                                      # Ctrl-C
                    termios.tcsetattr(fd, termios.TCSADRAIN, old); raise KeyboardInterrupt
                if o == 4:                                      # Ctrl-D (only exits on empty line)
                    if not buf:
                        termios.tcsetattr(fd, termios.TCSADRAIN, old); raise EOFError
                    continue
                if o in (8, 127):                               # backspace / DEL
                    if cur[0] > 0: del buf[cur[0] - 1]; cur[0] -= 1
                    continue
                if ch >= " " and o != 127:                      # printable (incl. unicode)
                    buf.insert(cur[0], ch); cur[0] += 1
            if submit:
                cur[0] = len(buf); render(final=True)           # cursor to end of the last row
                termios.tcsetattr(fd, termios.TCSADRAIN, old)
                sys.stdout.write("\n"); sys.stdout.flush()
                return "".join(buf)
            render()
    finally:
        try: termios.tcsetattr(fd, termios.TCSADRAIN, old)
        except Exception: pass

def prompt_input():
    w = term_width()
    if USAGE["ctx"]:
        print(dim("  context ~%s tokens  ·  %s out" % (_human(USAGE["ctx"]), _human(USAGE["out"]))))
    _mode_banner()
    print(orange("╭" + "─" * (w - 2) + "╮"))
    line = read_line()                     # '/' menu, shift+tab modes, history, no-wrap
    print(orange("╰" + "─" * (w - 2) + "╯"))
    _save_history_line(line.strip())
    return line

def expand_mentions(text):
    # @path/to/file -> inlines the file's contents (like Claude Code's @ mentions)
    def rep(m):
        path = m.group(1); fp = os.path.expanduser(path)
        if os.path.isfile(fp):
            try: body = open(fp, "r", errors="replace").read()[:20000]
            except Exception: return m.group(0)
            return "%s\n\n--- %s ---\n%s\n--- end %s ---" % (path, path, body, path)
        return m.group(0)
    text = re.sub(r"@([^\s]+)", rep, text)
    # #skill-name -> inlines a Skill's full instructions for THIS message, so you can
    # reference a skill mid-prompt instead of loading it first. Only expands when the
    # name actually matches an installed skill, so ordinary '#' (issue #5, C#) is left alone.
    def rep_skill(m):
        nm = m.group(1)
        s = _SKILLS.get(nm) or next((v for k, v in _SKILLS.items() if k.lower() == nm.lower()), None)
        if not s: return m.group(0)
        body, _ = tool_use_skill({"name": s["name"]})
        return '(use the "%s" skill — its instructions follow)\n\n--- Skill: %s ---\n%s\n--- end skill ---' % (
            s["name"], s["name"], body)
    return re.sub(r"#([A-Za-z0-9][A-Za-z0-9_-]*)", rep_skill, text)

# ----------------------------------------------------------------- config ----
PROVIDERS = {
    "anthropic":  {"kind": "anthropic", "env": "ANTHROPIC_API_KEY",  "base": None,
                   "model": "claude-sonnet-4-6",        "label": "Anthropic (Claude)"},
    "openrouter": {"kind": "openai",    "env": "OPENROUTER_API_KEY", "base": "https://openrouter.ai/api/v1",
                   "model": "anthropic/claude-sonnet-4.5", "label": "OpenRouter"},
    "ollama":     {"kind": "openai",    "env": "OLLAMA_API_KEY",     "base": "https://ollama.com/v1",
                   "model": "gpt-oss:120b",             "label": "Ollama Cloud"},
}
PROVIDER_ALIASES = {"ollama-cloud": "ollama", "ollamacloud": "ollama", "claude": "anthropic", "or": "openrouter"}

# Fallback model lists for the /model picker. When a key is set, /model fetches
# the provider's LIVE list from /v1/models (so Ollama shows its full cloud
# catalog); these are used only if that fetch fails or you're offline.
MODELS = {
    "anthropic":  ["claude-opus-4-8", "claude-sonnet-4-6", "claude-haiku-4-5-20251001"],
    "openrouter": ["anthropic/claude-sonnet-4.5", "anthropic/claude-opus-4.1", "openai/gpt-4o",
                   "google/gemini-2.5-pro", "deepseek/deepseek-chat", "qwen/qwen3-coder",
                   "meta-llama/llama-3.3-70b-instruct"],
    "ollama":     ["gpt-oss:120b", "gpt-oss:20b", "qwen3-coder:480b", "qwen3-coder-next",
                   "deepseek-v3.1:671b", "deepseek-v3.2", "deepseek-v4-pro", "deepseek-v4-flash",
                   "kimi-k2:1t", "kimi-k2-thinking", "kimi-k2.5", "kimi-k2.6", "kimi-k2.7-code",
                   "glm-4.6", "glm-4.7", "glm-5", "glm-5.1", "minimax-m2", "minimax-m2.1",
                   "minimax-m2.5", "minimax-m2.7", "minimax-m3", "qwen3-vl:235b", "qwen3.5:397b",
                   "qwen3-next:80b", "mistral-large-3:675b", "devstral-2:123b", "devstral-small-2:24b",
                   "nemotron-3-ultra", "nemotron-3-super", "nemotron-3-nano:30b",
                   "gemma3:27b", "gemma3:12b", "gemma3:4b", "gemma4:31b", "cogito-2.1:671b",
                   "ministral-3:14b", "ministral-3:8b", "ministral-3:3b", "gemini-3-flash-preview"],
}

_MODEL_CACHE = {}

def fetch_models(cfg):
    """Live model ids from the provider's /v1/models endpoint, or None."""
    try:
        if cfg["kind"] == "anthropic":
            url = "https://api.anthropic.com/v1/models"
            hdr = {"x-api-key": cfg["key"], "anthropic-version": "2023-06-01"}
        else:
            url = cfg["base"] + "/models"
            hdr = {"Authorization": "Bearer " + cfg["key"]}
        req = urllib.request.Request(url, headers=hdr)
        with urllib.request.urlopen(req, timeout=15) as r:
            j = json.loads(r.read().decode("utf-8", "replace"))
        data = j.get("data") or j.get("models") or []
        ids, seen = [], set()
        for m in data:
            mid = m.get("id") or m.get("name") or m.get("model")
            if mid and mid not in seen:
                seen.add(mid); ids.append(mid)
        return ids or None
    except Exception:
        return None

def provider_models(cfg, refresh=False):
    prov = cfg["provider"]
    if not refresh and prov in _MODEL_CACHE:
        return _MODEL_CACHE[prov]
    sys.stdout.write("  " + dim("fetching the live model list…")); sys.stdout.flush()
    live = fetch_models(cfg)
    sys.stdout.write("\r\033[K"); sys.stdout.flush()
    models = live if live else MODELS.get(prov, [])   # full live list (type to filter in the picker)
    _MODEL_CACHE[prov] = models
    return models

def file_config():
    p = os.path.expanduser("~/.vanta-code/config.json")
    if os.path.exists(p):
        try: return json.load(open(p))
        except Exception: return {}
    return {}

def make_cfg(provider, fc=None, use_env_model=True):
    p = PROVIDERS.get(provider)
    if not p: return None
    fc = fc or {}
    key = os.environ.get(p["env"]) or (fc.get("key", "") if fc.get("provider") == provider else "")
    if not key: return None
    model = (os.environ.get("VANTA_CODE_MODEL") if use_env_model else None) \
            or (fc.get("model") if fc.get("provider") == provider else None) or p["model"]
    return {"provider": provider, "kind": p["kind"], "key": key,
            "base": p["base"], "model": model, "label": p["label"]}

def load_config():
    fc = file_config()
    prov = fc.get("provider")
    if not prov:
        for cand in ("anthropic", "openrouter", "ollama"):
            if os.environ.get(PROVIDERS[cand]["env"]): prov = cand; break
    if not prov: return None
    return make_cfg(prov, fc)

def save_config(updates):
    path = os.path.expanduser("~/.vanta-code/config.json")
    d = file_config(); d.update(updates)
    try:
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "w") as f: json.dump(d, f, indent=2)
        try: os.chmod(path, 0o600)
        except Exception: pass
    except Exception as e:
        print(red("  could not write config: %s" % e))

def _saved_key(provider):
    fc = file_config()
    return fc.get("key") if fc.get("provider") == provider else None

def _ask_key(envname):
    # hidden input first; fall back to a visible prompt if the terminal can't hide it
    import getpass
    try: k = getpass.getpass("  " + orange("paste %s ❯ " % envname))
    except Exception: k = ""
    if not k.strip():
        try: k = input("  " + orange("paste %s (visible) ❯ " % envname))
        except Exception: k = ""
    return k.strip()

# ------------------------------------------------------ arrow-key menu (TTY) --
def _numbered_pick(title, rows):
    print(title)
    for i, r in enumerate(rows): print("  %d. %s" % (i + 1, r))
    try: n = int(input("  pick a number: ")) - 1
    except Exception: return None
    return n if 0 <= n < len(rows) else None

def select_menu(title, rows, idx=0):
    """Up/Down to move, type to filter, Enter to pick, Esc to cancel. Returns the
    ORIGINAL index of the chosen row (or None). Handles big lists (337 models)
    via a scrolling viewport + live substring filter. Falls back to a numbered
    prompt when there's no real terminal."""
    if not (sys.stdin.isatty() and sys.stdout.isatty()):
        return _numbered_pick(title, rows)
    try:
        import termios, tty
    except Exception:
        return _numbered_pick(title, rows)
    fd = sys.stdin.fileno()
    try: old = termios.tcgetattr(fd)
    except Exception: return _numbered_pick(title, rows)
    n_all = len(rows)
    plain = [re.sub(r"\033\[[0-9;]*m", "", r) for r in rows]   # for matching
    try: height = shutil.get_terminal_size().lines
    except Exception: height = 24
    vis = max(4, min(n_all, height - 6))
    query = [""]; cur = [min(idx, n_all - 1) if n_all else 0]; fil = [list(range(n_all))]; top = [0]
    def refilter():
        q = query[0].lower()
        fil[0] = list(range(n_all)) if not q else [i for i in range(n_all) if q in plain[i].lower()]
        cur[0] = 0; top[0] = 0
    print(title)
    def draw():
        m = len(fil[0])
        if cur[0] >= m: cur[0] = max(0, m - 1)
        if cur[0] < top[0]: top[0] = cur[0]
        elif cur[0] >= top[0] + vis: top[0] = cur[0] - vis + 1
        if top[0] > max(0, m - vis): top[0] = max(0, m - vis)
        if top[0] < 0: top[0] = 0
        for r in range(vis):
            mi = top[0] + r
            if mi < m:
                row = rows[fil[0][mi]]
                ptr = orange("❯ ") if mi == cur[0] else "  "
                sys.stdout.write("\r\033[K" + ptr + (bold(row) if mi == cur[0] else dim(row)) + "\n")
            else:
                sys.stdout.write("\r\033[K\n")
        flt = ("   filter: " + bold(query[0]) + "▏") if query[0] else "   (type to filter)"
        sys.stdout.write("\r\033[K" + dim("  %d/%d  ↑/↓ Enter · Esc%s" % ((cur[0] + 1) if m else 0, m, flt)) + "\n")
        sys.stdout.flush()
    draw()
    def _close(v):                                  # erase the whole menu on the way out
        sys.stdout.write("\033[%dA\r\033[J" % (vis + 2)); sys.stdout.flush()
        return v
    try:
        nw = termios.tcgetattr(fd)            # raw input: no canonical, no echo, no signals
        nw[3] = nw[3] & ~(termios.ICANON | termios.ECHO | termios.ISIG)
        termios.tcsetattr(fd, termios.TCSANOW, nw)
        while True:
            b = os.read(fd, 256)                   # big enough that batched arrows fit in one read
            if not b: continue
            if b[:1] == b"\x1b":                    # escape / arrows (a read may batch many)
                up = b.count(b"\x1b[A"); down = b.count(b"\x1b[B")
                if (up or down) and fil[0]:
                    cur[0] = (cur[0] + down - up) % len(fil[0])
                    sys.stdout.write("\033[%dA" % (vis + 1)); draw(); continue
                if len(b) >= 3 and b[1:2] == b"[":
                    continue                        # other CSI (←/→/Home) - ignore
                return _close(None)                 # bare Esc cancels
            # a single read can carry filter chars AND Enter/Ctrl-C together (terminals
            # batch input) - scan byte-by-byte so a trailing Enter still commits.
            changed = False
            for byte in bytearray(b):
                if byte in (10, 13):                # Enter -> commit the (refiltered) choice
                    if changed: refilter()
                    return _close(fil[0][cur[0]] if fil[0] else None)
                if byte == 3:                       # Ctrl-C
                    return _close(None)
                if byte in (8, 127):
                    if query[0]: query[0] = query[0][:-1]; changed = True
                elif 32 <= byte <= 126:
                    query[0] += chr(byte); changed = True
            if changed: refilter()
            sys.stdout.write("\033[%dA" % (vis + 1)); draw()
    finally:
        termios.tcsetattr(fd, termios.TCSADRAIN, old)

def skills_browser(names=None, title="Skills", saved=None, hint="enter to add"):
    """A polished, scrollable browser for Skills - colour hierarchy (names pop,
    descriptions recede), width-fit with ellipsis, full-block redraw + cleanup.
    ↑/↓ scroll, type to filter, Enter returns the chosen skill name, Esc cancels.
    `saved` is a set of names to flag with a ★ (already in My Skills)."""
    if names is None: names = list(_SKILLS)
    saved = saved or set()
    if not names: return None
    if not (sys.stdin.isatty() and sys.stdout.isatty()):
        for n in names: print("  " + orange(n) + dim("  " + (_SKILLS[n]["description"] or "")[:60]))
        return None
    try: import termios
    except Exception: return None
    fd = sys.stdin.fileno()
    try: old = termios.tcgetattr(fd)
    except Exception: return None
    W = term_width()
    try: H = shutil.get_terminal_size().lines
    except Exception: H = 24
    vis = max(5, min(len(names), H - 5))
    namew = min(22, max(len(n) for n in names) + 1)
    cur = [0]; top = [0]; query = [""]; fil = [list(range(len(names)))]; drawn = [0]
    def refilter():
        q = query[0].lower()
        fil[0] = [i for i, n in enumerate(names)
                  if not q or q in n.lower() or q in (_SKILLS[n]["description"] or "").lower()]
        cur[0] = 0; top[0] = 0
    def draw():
        m = len(fil[0])
        if cur[0] >= m: cur[0] = max(0, m - 1)
        if cur[0] < top[0]: top[0] = cur[0]
        elif cur[0] >= top[0] + vis: top[0] = cur[0] - vis + 1
        descw = max(12, W - namew - 7)
        out = [bold(orange("  " + title)) + dim("  %d" % len(names)) +
               dim("      ↑↓ scroll · type to filter · %s · esc" % hint)]
        for r in range(vis):
            mi = top[0] + r
            if mi < m:
                n = names[fil[0][mi]]; d = " ".join((_SKILLS[n]["description"] or "").split())
                if len(d) > descw: d = d[:descw - 1] + "…"
                nc = n[:namew].ljust(namew)
                star = orange("★") if n in saved else " "
                if mi == cur[0]:
                    out.append(orange("▸ ") + star + " " + bold(nc) + " " + d)
                else:
                    out.append("  " + star + " " + blue(nc) + " " + dim(d))
            else:
                out.append("")
        flt = ("  filter: " + bold(query[0])) if query[0] else ""
        out.append(dim("  %d/%d%s" % ((cur[0] + 1) if m else 0, m, flt)))
        if drawn[0]: sys.stdout.write("\033[%dA" % drawn[0])
        for ln in out: sys.stdout.write("\r\033[K" + ln + "\n")
        drawn[0] = len(out); sys.stdout.flush()
    def _close(v):
        if drawn[0]: sys.stdout.write("\033[%dA\r\033[J" % drawn[0]); sys.stdout.flush()
        return v
    draw()
    try:
        nw = termios.tcgetattr(fd); nw[3] = nw[3] & ~(termios.ICANON | termios.ECHO | termios.ISIG)
        termios.tcsetattr(fd, termios.TCSANOW, nw)
        while True:
            b = os.read(fd, 256)
            if not b: continue
            if b[:1] == b"\x1b":
                up = b.count(b"\x1b[A"); down = b.count(b"\x1b[B")
                if (up or down) and fil[0]:
                    cur[0] = (cur[0] + down - up) % len(fil[0]); draw(); continue
                if len(b) >= 3 and b[1:2] == b"[": continue
                return _close(None)
            changed = False
            for byte in bytearray(b):
                if byte in (10, 13):
                    if changed: refilter()
                    return _close(names[fil[0][cur[0]]] if fil[0] else None)
                if byte == 3: return _close(None)
                if byte in (8, 127):
                    if query[0]: query[0] = query[0][:-1]; changed = True
                elif 32 <= byte <= 126:
                    query[0] += chr(byte); changed = True
            if changed: refilter()
            draw()
    finally:
        termios.tcsetattr(fd, termios.TCSADRAIN, old)

def theme_swatch(nm):
    th = THEMES[nm]
    if not COLOR: return "%-10s ########" % nm
    blocks = "".join("\033[%sm█" % _code(_at(th["grad"], k / 7.0)) for k in range(8)) + "\033[0m"
    return "\033[%sm%-10s\033[0m %s" % (_code(th["accent"]), nm, blocks)

def do_theme_menu(cfg):
    names = list(THEMES)
    idx = names.index(THEME["name"]) if THEME["name"] in names else 0
    sel = select_menu(orange("Choose a theme") + dim("   ↑/↓ then Enter · Esc to cancel"),
                      [theme_swatch(n) for n in names], idx)
    if sel is None:
        print(dim("  (kept " + THEME["name"] + ")")); return
    set_theme(names[sel]); save_config({"theme": names[sel]})
    print(); banner(cfg)   # re-show the banner so you see the new colors instantly

def _provider_url(p):
    return {"anthropic": "console.anthropic.com/settings/keys",
            "openrouter": "openrouter.ai/keys",
            "ollama": "ollama.com/settings/keys"}.get(p, "")

def first_run_setup():
    """No key anywhere -> walk the user through picking a provider and pasting a
    key, right here in the CLI. Returns a cfg or None if they skip."""
    print()
    for line in VANTA_ART: print("  " + grad_line(line))
    print("  " + orange("c o d e") + dim("   ·   first-time setup"))
    print()
    print("  vcode runs on " + bold("your own AI key") + " — pick a provider and paste a key.")
    print()
    names = list(PROVIDERS)
    rows = []
    for pk in names:
        pv = PROVIDERS[pk]
        has = green("● key found") if (os.environ.get(pv["env"]) or _saved_key(pk)) else grey("○ needs a key")
        rows.append("%-11s %-22s %s" % (pk, pv["label"], has))
    sel = select_menu(orange("Choose your AI provider") + dim("   ↑/↓ then Enter · Esc to skip"), rows, 0)
    if sel is None: return None
    target = names[sel]; pv = PROVIDERS[target]
    if not (os.environ.get(pv["env"]) or _saved_key(target)):
        print(dim("  get a key at: ") + orange(_provider_url(target)))
        key = _ask_key(pv["env"])
        if not key:
            print(dim("  no key entered.")); return None
        save_config({"provider": target, "key": key, "model": pv["model"]})
        ok = _saved_key(target) == key
        print(green("  ✓ saved to ~/.vanta-code/config.json — change anytime with /provider or /key") if ok
              else red("  ⚠ could not save the key to ~/.vanta-code/config.json (check permissions)"))
    else:
        save_config({"provider": target})
    return make_cfg(target, file_config())

def do_provider_menu(cfg):
    keys = list(PROVIDERS.keys())
    rows = []
    for pk in keys:
        pv = PROVIDERS[pk]
        has = green("● key set") if (os.environ.get(pv["env"]) or _saved_key(pk)) else grey("○ no key yet")
        rows.append("%-11s %-20s %s" % (pk, pv["label"], has))
    idx = keys.index(cfg["provider"]) if cfg.get("provider") in keys else 0
    sel = select_menu(orange("Choose a provider") + dim("   ↑/↓ then Enter · Esc to cancel"), rows, idx)
    if sel is None:
        print(dim("  (cancelled)")); return cfg
    target = keys[sel]; pv = PROVIDERS[target]
    if not (os.environ.get(pv["env"]) or _saved_key(target)):
        key = _ask_key(pv["env"])
        if not key:
            print(dim("  (no key entered)")); return cfg
        save_config({"provider": target, "key": key, "model": pv["model"]})
        print(green("  ✓ saved your %s key" % target) if _saved_key(target) == key
              else red("  ⚠ could not save the key (check ~/.vanta-code permissions)"))
    else:
        save_config({"provider": target})
    nc = make_cfg(target, file_config(), use_env_model=False)
    if not nc:
        print(red("  could not load %s" % target)); return cfg
    print(dim("  provider → " + nc["provider"] + " · " + nc["model"]))
    return do_model_menu(nc)

def do_model_menu(cfg):
    models = provider_models(cfg)
    if not models:
        print(dim("  no models found for " + cfg["provider"] + " — set one with /model <id>")); return cfg
    idx = models.index(cfg["model"]) if cfg["model"] in models else 0
    sel = select_menu(orange("Choose a model") + dim("   (" + cfg["provider"] + ", %d models)  ↑/↓ then Enter" % len(models)), models, idx)
    if sel is None:
        print(dim("  (kept " + cfg["model"] + ")")); return cfg
    cfg["model"] = models[sel]; save_config({"provider": cfg["provider"], "model": cfg["model"]})
    print(dim("  model → " + cfg["model"]))
    return cfg

def no_key_screen():
    print()
    box([orange("✻ ") + bold("Vanta Code") + dim("  needs an API key")], term_width())
    print()
    print("  Vanta Code thinks with an LLM, so it uses " + bold("your own key") + ". Set one of:")
    print()
    print("    " + green('export ANTHROPIC_API_KEY="sk-ant-..."') + dim("   # uses Claude directly"))
    print("    " + green('export OPENROUTER_API_KEY="sk-or-..."') + dim("    # uses OpenRouter"))
    print("    " + green('export OLLAMA_API_KEY="..."') + dim("              # uses Ollama Cloud"))
    print()
    print("  Then run " + bold("vanta-code") + " again, and use " + bold("/provider") + " to switch.")
    print()

# ------------------------------------------------------------------ main -----
def main():
    args = sys.argv[1:]
    if "--version" in args or "-v" in args:
        print("vcode " + VERSION); return
    if "--help" in args or "-h" in args:
        print("vcode - a terminal coding agent that speaks Vanta.\n")
        print("  Usage: vcode             start the interactive agent")
        print("         vcode --continue  resume your last session")
        print("         vcode --version   print version\n")
        print("  Needs ANTHROPIC_API_KEY or OPENROUTER_API_KEY in your environment.")
        return

    cfg = load_config()
    if not cfg and sys.stdin.isatty() and sys.stdout.isatty():
        cfg = first_run_setup()                       # add a key right here in the CLI
    if not cfg:
        no_key_screen(); return
    set_theme(file_config().get("theme", "ember"))   # restore the saved theme
    _seed_skills()                                    # ship example Vanta skills on first run
    refresh_context()                                 # auto-load VANTA.md/AGENTS.md/CLAUDE.md + skills
    load_myskills()                                   # restore the user's pinned My Skills
    try:
        import readline, atexit
        _hist = os.path.expanduser("~/.vanta-code/history")
        try: os.makedirs(os.path.dirname(_hist), exist_ok=True)
        except Exception: pass
        try: readline.read_history_file(_hist)
        except Exception: pass
        readline.set_history_length(1000)
        def _savehist():
            try: readline.write_history_file(_hist)
            except Exception: pass
        atexit.register(_savehist)
        # tab-completion: slash commands at line start, else @file / path completion
        import glob as _glob
        _SLASH = ["/help", "/clear", "/compact", "/cost", "/resume", "/init", "/themes",
                  "/provider", "/key", "/model", "/auto", "/cwd", "/exit", "/quit"]
        def _complete(word, state):
            buf = readline.get_line_buffer().lstrip()
            opts = []
            if buf.startswith("/") and " " not in buf:
                opts = [c + " " for c in _SLASH if c.startswith(word)]
            else:
                at = word.startswith("@")
                pref = word[1:] if at else word
                try:
                    for m in sorted(_glob.glob(os.path.expanduser(pref) + "*"))[:40]:
                        opts.append(("@" if at else "") + m + ("/" if os.path.isdir(m) else ""))
                except Exception:
                    opts = []
            return opts[state] if state < len(opts) else None
        readline.set_completer_delims(" \t\n")
        readline.set_completer(_complete)
        if "libedit" in (getattr(readline, "__doc__", "") or ""):
            readline.parse_and_bind("bind ^I rl_complete")   # macOS libedit
        else:
            readline.parse_and_bind("tab: complete")          # GNU readline
    except Exception:
        pass
    _load_history()
    SESSION["start"] = time.time()

    banner(cfg)
    history = []
    if "--continue" in args or "-c" in args:
        s = load_session()
        if s: history[:] = s; print(dim("  ↻ continued previous session (%d messages)\n" % len(s)))
    while True:
        try:
            line = prompt_input().strip()
        except EOFError:
            print("\n" + dim("  bye.")); return            # Ctrl-D exits
        except KeyboardInterrupt:
            print(dim("  (use Ctrl-D to exit)")); continue  # Ctrl-C just clears the line
        if not line:
            continue
        if line == '"""':                              # paste a multi-line block until the next """
            buf = []
            _mlp = dim("  ┃ ") if (COLOR and not _is_libedit()) else "  ┃ "
            while True:
                try: more = input(_mlp)
                except EOFError: break
                if more.strip() == '"""': break
                buf.append(more)
            block = "\n".join(buf).strip()
            if not block: continue
            try: agent_turn(cfg, history, expand_mentions(block))
            except KeyboardInterrupt: print("\n" + dim("  (interrupted)"))
            save_session(history); print(); continue
        if line.startswith("!"):                       # run a shell command directly
            cmd = line[1:].strip()
            if cmd:
                try:
                    r = subprocess.run(cmd, shell=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=120)
                    o = r.stdout.decode("utf-8", "replace")
                    print(o.rstrip() if o.strip() else dim("  (no output)"))
                    history.append({"role": "user", "content": "I ran `%s`; output:\n%s" % (cmd, o[:4000])})
                    save_session(history)
                except Exception as e:
                    print(red("  " + str(e)))
            continue
        # A leading '/' is a command ONLY if it's command-shaped (/word) and isn't an
        # actual path - so "/Users/me/file.pdf" or "/tmp" is sent to the agent, not
        # rejected as an unknown command.
        _first = line.split(" ", 1)[0]
        _is_cmd = (line.startswith("/") and re.match(r"^/[A-Za-z0-9][\w-]*$", _first)
                   and not os.path.exists(os.path.expanduser(_first)))
        if _is_cmd:
            cmd = line[1:].split(" ", 1)
            name = cmd[0].lower(); rest = cmd[1].strip() if len(cmd) > 1 else ""
            if name in ("exit", "quit"): print(dim("  bye.")); return
            elif name == "help": print(HELP % MODE["v"])
            elif name == "clear": history = []; print(dim("  context cleared."))
            elif name == "compact":
                if history: history[:] = compact_history(cfg, history)
                else: print(dim("  nothing to compact yet."))
            elif name == "init":
                agent_turn(cfg, history, expand_mentions("Look around this project (use glob/list_files/read_file) and write a short VANTA.md in the current folder: what it is, how to run it, key files, and any Vanta conventions used. Then confirm you created it."))
                refresh_context()
            elif name == "resume":
                s = load_session()
                if s: history[:] = s; print(dim("  resumed %d messages from your last session." % len(s)))
                else: print(dim("  no saved session found."))
            elif name == "key":
                pv = PROVIDERS[cfg["provider"]]
                k = _ask_key(pv["env"])
                if k:
                    save_config({"provider": cfg["provider"], "key": k}); cfg["key"] = k
                    print(green("  ✓ key saved for " + cfg["provider"]) if _saved_key(cfg["provider"]) == k
                          else red("  ⚠ could not save the key (check ~/.vanta-code permissions)"))
                else: print(dim("  (cancelled)"))
            elif name == "auto":
                MODE["v"] = "default" if MODE["v"] == "auto" else "auto"; print(dim("  mode: " + MODE["v"]))
            elif name == "plan":
                MODE["v"] = "default" if MODE["v"] == "plan" else "plan"; print(dim("  mode: " + MODE["v"]))
            elif name == "mode":
                MODE["v"] = MODE_ORDER[(MODE_ORDER.index(MODE["v"]) + 1) % len(MODE_ORDER)]; print(dim("  mode: " + MODE["v"]))
            elif name in ("themes", "theme"): do_theme_menu(cfg)
            elif name == "model":
                if not rest or rest.lower() == "refresh":
                    if rest.lower() == "refresh": _MODEL_CACHE.pop(cfg["provider"], None)
                    cfg = do_model_menu(cfg)
                else:
                    models = provider_models(cfg)
                    if rest.isdigit() and models:
                        idx = int(rest) - 1
                        if 0 <= idx < len(models):
                            cfg["model"] = models[idx]; save_config({"provider": cfg["provider"], "model": cfg["model"]})
                            print(dim("  model -> " + cfg["model"]))
                        else:
                            print(red("  pick 1-%d, or type a model name" % len(models)))
                    else:
                        cfg["model"] = rest; save_config({"provider": cfg["provider"], "model": rest}); print(dim("  model -> " + rest))
            elif name == "provider":
                if not rest:
                    cfg = do_provider_menu(cfg)
                else:
                    target = PROVIDER_ALIASES.get(rest.lower(), rest.lower())
                    if target not in PROVIDERS:
                        print(red("  unknown provider '%s'. options: %s" % (rest, ", ".join(PROVIDERS))))
                    else:
                        nc = make_cfg(target, file_config(), use_env_model=False)
                        if not nc:
                            print(red("  no key for %s — set %s, or run /provider to paste one" % (target, PROVIDERS[target]["env"])))
                        else:
                            cfg = nc; save_config({"provider": target})
                            print(dim("  provider -> " + cfg["provider"] + " · " + cfg["model"]))
            elif name == "cwd":
                if rest:
                    try: os.chdir(os.path.expanduser(rest)); print(dim("  cwd -> " + os.getcwd()))
                    except Exception as e: print(red("  " + str(e)))
                else: print(dim("  cwd: " + os.getcwd()))
            elif name == "cost":
                el = int(time.time() - SESSION["start"]) if SESSION["start"] else 0
                print(dim("  session  %dm %02ds  ·  %d turns  ·  %d tool calls" %
                          (el // 60, el % 60, SESSION["turns"], SESSION["tools"])))
                print(dim("  tokens   ~%s in context  ·  %s generated" %
                          (_human(USAGE["ctx"]), _human(USAGE["out"]))))
            elif name in ("skills", "skill"):
                if rest.split(" ", 1)[0] in ("install", "add", "get"):
                    parts = rest.split(None, 1)
                    repo = parts[1].strip() if len(parts) > 1 else "https://github.com/anthropics/skills"
                    print(dim("  fetching skills from " + repo + " ..."))
                    n, info = install_skills(repo)
                    if n: print(green("  ✓ installed %d skill(s)" % n)); refresh_context()
                    else: print(red("  couldn't install: " + str(info)))
                elif rest.split(" ", 1)[0] in ("remove", "rm", "forget"):
                    parts = rest.split(None, 1)
                    nm = parts[1].strip() if len(parts) > 1 else ""
                    if nm and remove_myskill(nm): print(dim("  removed " + nm + " from My Skills."))
                    else: print(dim("  not in My Skills. (your skills: %s)" % (", ".join(my_skill_names()) or "none")))
                else:
                    refresh_context()
                    if not _SKILLS:
                        print(dim("  no skills yet. Try  /skills install  (grabs Anthropic's skills),"))
                        print(dim("  or drop a SKILL.md folder into ~/.vanta-code/skills/ or ~/.claude/skills/."))
                    else:
                        picked = skills_browser(saved=set(my_skill_names()))   # ★ marks pinned ones
                        if picked and picked in _SKILLS:
                            s = _SKILLS[picked]; newly = add_myskill(picked)
                            if newly:
                                print("  " + orange("★ ") + bold(orange("pinned ")) + bold(s["name"]) + dim("  to My Skills"))
                            else:
                                print("  " + orange("★ ") + bold(s["name"]) + dim("  is already in My Skills"))
                            d = " ".join((s["description"] or "(no description)").split())
                            print(dim("  " + (d[:300] + ("…" if len(d) > 300 else ""))))
                            print(dim("  use it:  /myskills · type /%s · or inline in a prompt with #%s" % (s["name"], s["name"])))
                        print(dim("  add more:  /skills install [owner/repo or url]"))
            elif name in ("myskills", "myskill"):
                refresh_context()
                mine = my_skill_names()
                if not mine:
                    print(dim("  no pinned skills yet. Open  /skills , highlight one and press Enter to pin it here."))
                else:
                    picked = skills_browser(mine, title="My Skills", saved=set(mine), hint="enter to use")
                    if picked and picked in _SKILLS:
                        load_skill_into(history, picked)
                    else:
                        print(dim("  your skills:  " + "  ".join("/" + n for n in mine)))
                        print(dim("  remove one with  /skills remove <name>"))
            elif name in {n.lower() for n in _SKILLS}:             # typed /<any-skill> -> load it
                real = next(n for n in _SKILLS if n.lower() == name)
                load_skill_into(history, real)
            else: print(dim("  unknown command. /help for the list."))
            continue
        try:
            agent_turn(cfg, history, expand_mentions(line))
        except KeyboardInterrupt:
            print("\n" + dim("  (interrupted)"))
        save_session(history)
        print()

if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        print()
