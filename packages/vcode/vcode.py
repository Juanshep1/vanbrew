#!/usr/bin/env python3
# vanta-code - a terminal coding agent that specializes in the Vanta language,
# styled to look like Claude Code. Uses YOUR OWN API key (Anthropic or
# OpenRouter) from the environment. Single file, standard library only.
from __future__ import print_function
import os, sys, json, time, threading, subprocess, shutil, tempfile, re
import urllib.request, urllib.error

VERSION = "1.0"

# ---------------------------------------------------------------- colours ----
COLOR = sys.stdout.isatty() and os.environ.get("TERM") not in (None, "", "dumb")
def _c(s, code):
    return ("\033[%sm%s\033[0m" % (code, s)) if COLOR else s
ORANGE = "38;2;217;119;87"      # Claude's terracotta
TAN    = "38;2;200;160;130"
DIM    = "2;37"
GREY   = "38;2;140;140;150"
GREEN  = "38;2;126;192;80"
RED    = "38;2;229;90;90"
BLUE   = "38;2;120;160;255"
BOLD   = "1"
def orange(s): return _c(s, ORANGE)
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
GRAD_STOPS = [(99, 102, 241), (168, 85, 247), (217, 70, 160), (240, 118, 92), (245, 176, 86)]
def _lerp(a, b, t): return tuple(int(a[k] + (b[k] - a[k]) * t) for k in range(3))
def _grad_at(t):
    if t <= 0: return GRAD_STOPS[0]
    if t >= 1: return GRAD_STOPS[-1]
    seg = t * (len(GRAD_STOPS) - 1); i = int(seg); return _lerp(GRAD_STOPS[i], GRAD_STOPS[i + 1], seg - i)
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
    try: return min(shutil.get_terminal_size().columns, 90)
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
                                   dim(" (%ss" % el), dim("· esc to interrupt)"))
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
- Strings: `upper`, `lower`, `slice(s, a, b)`, `replace(s, old, new)`, `split(s, sep)`, `join(list, sep)`, `starts_with`, `ends_with`, `length`.
- String interpolation: `"hi {name}"` inserts the value of name. To put a LITERAL brace in a string, double it: `{{` and `}}` (this matters a LOT when emitting CSS/JS).
- Web + system builtins: `serve(port, handler)` (handler takes a request map, gives back text or a map {status, body, type, headers}); `http_get(url[, headers])` and `http_post(url, body[, headers])` -> {status, body, headers}; `read_file`/`write_file`/`append_file`/`list_dir`/`make_dir`/`remove_path`; `from_json`/`to_json`; `run(cmd)` and `shell(cmd)`; `open_url(url)`; `home_dir()`, `path_join(...)`, `dirname`, `basename`; `now()`, `today()`, `clock()`; `run_vanta(code)` runs Vanta source in-process; `ask(prompt)` reads a line of input; `os_name()`.
- Every block (`if`, `for each`, `while`, `to`) closes with `end`. There are no curly-brace code blocks and no semicolons. `times` is reserved - don't use it as a variable.

# How to work
- You have FULL ACCESS to this computer. You can create folders ANYWHERE (make_dir), read/write/move/delete any file, and run any shell command (bash). You are NOT limited to the current directory - use absolute paths (e.g. ~/projects/foo/app.va, /Users/.../). Coding Vanta works in any location.
- Your main job is BUILDING FROM SCRATCH. When the user asks you to make / build / create / write / code an app or program, WRITE it yourself with write_file - produce complete, original, working Vanta code. Do NOT reuse, copy, or just run a file that already exists, and do NOT go hunting for an existing .va to run. "Make a tip calculator" means write brand-new .va code for one - never run ~/tipjar.va or anything pre-made unless the user EXPLICITLY says "run the existing X".
- For a new project, MAKE A DEDICATED FOLDER for it (make_dir, e.g. ~/vanta/<name>/) and put the .va plus any assets inside, unless the user says where. Then launch it so the user sees it: a visual/web app -> run_app (pops a movable window); a plain script -> run_vanta (console output). Read any error, fix the .va, and re-run until it works.
- Only use run_app/run_vanta on an EXISTING file when the user explicitly says "run/open <that file>". Otherwise you are creating, not fetching.
- Keep answers tight: a sentence on what you built, then the result.

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

Put real inputs/buttons in <div id='body'> and their logic in the <script> (use `{{`/`}}` for braces). For a backend app instead, write serve(PORT, handler) returning HTML/JSON; run_app will launch it and open its port."""

# ------------------------------------------------------------------- tools ---
TOOLS = [
    {"name": "read_file", "description": "Read a UTF-8 text file and return its contents.",
     "input_schema": {"type": "object", "properties": {"path": {"type": "string"}}, "required": ["path"]}},
    {"name": "write_file", "description": "Write (create or overwrite) a text file. Use for .va files and any code.",
     "input_schema": {"type": "object", "properties": {"path": {"type": "string"}, "content": {"type": "string"}}, "required": ["path", "content"]}},
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
]

def find_vanta():
    for p in [shutil.which("vanta"), os.path.expanduser("~/.vanbrew/bin/vanta")]:
        if p and os.path.exists(p): return p
    return None

AUTO = {"on": False}  # session auto-approve toggle

def _confirm(action):
    if AUTO["on"]: return True
    try:
        sys.stdout.write("  %s %s " % (orange("Proceed?"), dim("(y / N / a=always)")))
        sys.stdout.flush()
        ans = sys.stdin.readline().strip().lower()
    except Exception:
        return False
    if ans == "a": AUTO["on"] = True; return True
    return ans in ("y", "yes")

def tool_read_file(a):
    p = os.path.expanduser(a["path"])
    with open(p, "r") as f: txt = f.read()
    if len(txt) > 60000: txt = txt[:60000] + "\n... (truncated)"
    return txt, "%d lines" % (txt.count("\n") + 1)

def tool_write_file(a):
    p = os.path.expanduser(a["path"]); content = a.get("content", "")
    d = os.path.dirname(p)
    if d and not os.path.isdir(d): os.makedirs(d)
    with open(p, "w") as f: f.write(content)
    return "Wrote %s" % p, "%d lines" % (content.count("\n") + 1)

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

def tool_run_app(a):
    path = os.path.expanduser(a["path"])
    if not os.path.exists(path): return "no such file: " + path, "missing"
    v = find_vanta()
    if not v: return "vanta CLI not found — run: vanbrew install vanta", "no vanta"
    try: src = open(path).read()
    except Exception as e: return "could not read %s: %s" % (path, e), "error"
    chrome = find_chrome()
    if "serve(" in src:   # a web server: launch it, open its port in a movable window
        m = (re.search(r"serve\(\s*(\d{2,5})", src) or re.search(r"PORT\s+be\s+(\d{2,5})", src)
             or re.search(r"\bbe\s+(\d{4,5})\b", src))
        port = m.group(1) if m else "8080"
        subprocess.Popen([v, path], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        url = "http://localhost:%s/" % port
        time.sleep(1.6)
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
            "list_files": tool_list_files, "make_dir": tool_make_dir,
            "move_path": tool_move_path, "delete_path": tool_delete_path,
            "run_vanta": tool_run_vanta, "run_app": tool_run_app, "bash": tool_bash}

def tool_label(name, a):
    if name == "read_file":  return "Read(%s)" % a.get("path", "")
    if name == "write_file": return "Write(%s)" % a.get("path", "")
    if name == "list_files": return "List(%s)" % a.get("path", ".")
    if name == "make_dir":   return "Mkdir(%s)" % a.get("path", "")
    if name == "move_path":  return "Move(%s -> %s)" % (a.get("from", ""), a.get("to", ""))
    if name == "delete_path":return "Delete(%s)" % a.get("path", "")
    if name == "run_vanta":  return "Run(%s)" % a.get("path", "")
    if name == "run_app":    return "Open app(%s)" % a.get("path", "")
    if name == "bash":       return "Bash(%s)" % a.get("command", "")[:50]
    return "%s(%s)" % (name, a)

def run_tool(name, a):
    print(orange("⏺ ") + bold(tool_label(name, a)))
    try:
        result, summary = DISPATCH[name](a)
    except Exception as e:
        result, summary = ("Error: %s" % e), "error"
    print("  " + dim("⎿  " + summary))
    return result

# --------------------------------------------------------------- LLM client --
def http_json(url, payload, headers, timeout=120):
    data = json.dumps(payload).encode("utf-8")
    req = urllib.request.Request(url, data=data, headers=headers, method="POST")
    try:
        with urllib.request.urlopen(req, timeout=timeout) as r:
            return json.loads(r.read().decode("utf-8", "replace"))
    except urllib.error.HTTPError as e:
        body = e.read().decode("utf-8", "replace")
        raise RuntimeError("API error %s: %s" % (e.code, body[:500]))
    except urllib.error.URLError as e:
        raise RuntimeError("could not reach the API: %s" % getattr(e, "reason", e))

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
            msg = {"role": "assistant", "content": text or None}
            if calls: msg["tool_calls"] = calls
            out.append(msg)
        else:  # user with tool_result blocks
            for b in m["content"]:
                if b["type"] == "tool_result":
                    out.append({"role": "tool", "tool_call_id": b["tool_use_id"], "content": b["content"]})
                elif b["type"] == "text":
                    out.append({"role": "user", "content": b["text"]})
    return out

def call_llm(cfg, history):
    """Return a normalized assistant message: {content:[blocks], stop:'tool'|'end'}."""
    if cfg["kind"] == "anthropic":
        payload = {"model": cfg["model"], "max_tokens": 4096, "system": SYSTEM,
                   "messages": history, "tools": TOOLS}
        headers = {"x-api-key": cfg["key"], "anthropic-version": "2023-06-01",
                   "content-type": "application/json"}
        j = http_json("https://api.anthropic.com/v1/messages", payload, headers)
        blocks = j.get("content", [])
        stop = "tool" if j.get("stop_reason") == "tool_use" else "end"
        return {"content": blocks, "stop": stop}
    else:  # openai-compatible (openrouter)
        oai_tools = [{"type": "function", "function": {"name": t["name"], "description": t["description"],
                      "parameters": t["input_schema"]}} for t in TOOLS]
        msgs = [{"role": "system", "content": SYSTEM}] + to_openai_msgs(history)
        payload = {"model": cfg["model"], "max_tokens": 4096, "messages": msgs,
                   "tools": oai_tools, "tool_choice": "auto"}
        headers = {"Authorization": "Bearer " + cfg["key"], "content-type": "application/json",
                   "HTTP-Referer": "https://github.com/Juanshep1/vanbrew", "X-Title": "Vanta Code"}
        j = http_json(cfg["base"] + "/chat/completions", payload, headers)
        m = j["choices"][0]["message"]
        blocks = []
        if m.get("content"): blocks.append({"type": "text", "text": m["content"]})
        for tc in (m.get("tool_calls") or []):
            try: inp = json.loads(tc["function"]["arguments"] or "{}")
            except Exception: inp = {}
            blocks.append({"type": "tool_use", "id": tc["id"], "name": tc["function"]["name"], "input": inp})
        stop = "tool" if (m.get("tool_calls")) else "end"
        return {"content": blocks, "stop": stop}

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

def print_assistant(text):
    w = term_width() - 2
    body = wrap(text.strip(), w)
    first = True
    for ln in body.split("\n"):
        if first:
            print(orange("⏺ ") + ln); first = False
        else:
            print("  " + ln)

def agent_turn(cfg, history, user_text):
    history.append({"role": "user", "content": user_text})
    for _ in range(60):
        sp = Spinner(SPIN_WORDS[int(time.time()) % len(SPIN_WORDS)]).start()
        try:
            resp = call_llm(cfg, history)
        except Exception as e:
            sp.stop(); print(red("⏺ " + str(e))); return
        sp.stop()
        history.append({"role": "assistant", "content": resp["content"]})
        # print any text, run any tools
        results = []
        for b in resp["content"]:
            if b["type"] == "text" and b["text"].strip():
                print_assistant(b["text"])
            elif b["type"] == "tool_use":
                out = run_tool(b["name"], b.get("input", {}))
                results.append({"type": "tool_result", "tool_use_id": b["id"], "content": out})
        if resp["stop"] != "tool" or not results:
            return
        history.append({"role": "user", "content": results})
    print(dim("  (stopped after too many steps)"))

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
    print(dim("  /help for commands   ·   ask me to build something   ·   Ctrl-D to exit"))
    print()

HELP = """  Commands:
    /help            show this help
    /clear           start a fresh conversation
    /provider [name] list providers, or switch: anthropic | openrouter | ollama
    /model [n|name]  list models and pick one (/model 2), or set any id
    /auto            toggle auto-approve for writes & shell (currently: %s)
    /cwd <path>      change working directory
    /exit, /quit     leave

  Just type what you want, e.g.:
    "write a fizzbuzz in vanta and run it"
    "make a web app that serves a todo list on port 8123"
    "read snake.va and explain what it does"
"""

def prompt_input():
    w = term_width()
    print(orange("╭" + "─" * (w - 2) + "╮"))
    sys.stdout.write(orange("│ ") + orange("› "))
    sys.stdout.flush()
    line = sys.stdin.readline()
    if line == "":  # EOF
        raise EOFError
    print(orange("╰" + "─" * (w - 2) + "╯"))
    return line.rstrip("\n")

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
    # OpenRouter lists hundreds of models — too many to scroll, so keep curated.
    if live and not (prov == "openrouter" and len(live) > 60):
        models = live
    else:
        models = MODELS.get(prov, [])
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
    dirp = os.path.dirname(path)
    if not os.path.isdir(dirp): os.makedirs(dirp)
    with open(path, "w") as f: json.dump(d, f, indent=2)
    try: os.chmod(path, 0o600)
    except Exception: pass

def _saved_key(provider):
    fc = file_config()
    return fc.get("key") if fc.get("provider") == provider else None

# ------------------------------------------------------ arrow-key menu (TTY) --
def _numbered_pick(title, rows):
    print(title)
    for i, r in enumerate(rows): print("  %d. %s" % (i + 1, r))
    try: n = int(input("  pick a number: ")) - 1
    except Exception: return None
    return n if 0 <= n < len(rows) else None

def select_menu(title, rows, idx=0):
    """Up/Down (or j/k) to move, Enter to pick, Esc/q to cancel. Returns index
    or None. Falls back to a numbered prompt when there's no real terminal.
    Reads raw bytes with os.read so terminal escape sequences arrive intact."""
    if not (sys.stdin.isatty() and sys.stdout.isatty()):
        return _numbered_pick(title, rows)
    try:
        import termios, tty
    except Exception:
        return _numbered_pick(title, rows)
    fd = sys.stdin.fileno()
    try: old = termios.tcgetattr(fd)
    except Exception: return _numbered_pick(title, rows)
    n = len(rows)
    try: height = shutil.get_terminal_size().lines
    except Exception: height = 24
    vis = max(4, min(n, height - 5))     # how many rows are visible at once
    top = [0]
    print(title)
    def draw():
        if idx < top[0]: top[0] = idx
        elif idx >= top[0] + vis: top[0] = idx - vis + 1
        top[0] = max(0, min(top[0], max(0, n - vis)))
        for r in range(vis):
            mi = top[0] + r
            if mi < n:
                ptr = orange("❯ ") if mi == idx else "  "
                sys.stdout.write("\r\033[K" + ptr + (bold(rows[mi]) if mi == idx else dim(rows[mi])) + "\n")
            else:
                sys.stdout.write("\r\033[K\n")
        tail = ("  ·  %d-%d of %d" % (top[0] + 1, min(top[0] + vis, n), n)) if n > vis else ""
        sys.stdout.write("\r\033[K" + dim("  %d/%d  ↑/↓ Enter · Esc%s" % (idx + 1, n, tail)) + "\n")
        sys.stdout.flush()
    draw()
    try:
        tty.setcbreak(fd)
        while True:
            b = os.read(fd, 6)          # an arrow arrives as b'\x1b[A' in one burst
            if not b: continue
            if b in (b"\r", b"\n"): return idx
            if b in (b"\x03", b"q", b"\x1b"): return None   # Ctrl-C / q / bare Esc
            if b[:2] == b"\x1b[":
                k = b[2:3]
                if k == b"A": idx = (idx - 1) % n
                elif k == b"B": idx = (idx + 1) % n
                else: continue
            elif b == b"k": idx = (idx - 1) % n
            elif b == b"j": idx = (idx + 1) % n
            else: continue
            sys.stdout.write("\033[%dA" % (vis + 1)); draw()
    finally:
        termios.tcsetattr(fd, termios.TCSADRAIN, old)

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
        import getpass
        print(dim("  paste your %s and press Enter (input hidden), blank to cancel:" % pv["env"]))
        try: key = getpass.getpass("  " + orange("key❯ "))
        except Exception: key = ""
        if not key.strip():
            print(dim("  (no key entered)")); return cfg
        save_config({"provider": target, "key": key.strip(), "model": pv["model"]})
        print(green("  ✓ saved your %s to ~/.vanta-code/config.json (chmod 600)") % target)
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
        print("  Usage: vanta-code            start the interactive agent")
        print("         vanta-code --version  print version\n")
        print("  Needs ANTHROPIC_API_KEY or OPENROUTER_API_KEY in your environment.")
        return

    cfg = load_config()
    if not cfg:
        no_key_screen(); return

    banner(cfg)
    history = []
    while True:
        try:
            line = prompt_input().strip()
        except (EOFError, KeyboardInterrupt):
            print("\n" + dim("  bye.")); return
        if not line:
            continue
        if line.startswith("/"):
            cmd = line[1:].split(" ", 1)
            name = cmd[0].lower(); rest = cmd[1].strip() if len(cmd) > 1 else ""
            if name in ("exit", "quit"): print(dim("  bye.")); return
            elif name == "help": print(HELP % ("on" if AUTO["on"] else "off"))
            elif name == "clear": history = []; print(dim("  context cleared."))
            elif name == "auto": AUTO["on"] = not AUTO["on"]; print(dim("  auto-approve %s." % ("on" if AUTO["on"] else "off")))
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
            else: print(dim("  unknown command. /help for the list."))
            continue
        try:
            agent_turn(cfg, history, line)
        except KeyboardInterrupt:
            print("\n" + dim("  (interrupted)"))
        print()

if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        print()
