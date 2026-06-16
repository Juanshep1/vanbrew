#!/usr/bin/env python3
# vanta-code - a terminal coding agent that specializes in the Vanta language,
# styled to look like Claude Code. Uses YOUR OWN API key (Anthropic or
# OpenRouter) from the environment. Single file, standard library only.
from __future__ import print_function
import os, sys, json, time, threading, subprocess, shutil, tempfile
import urllib.request, urllib.error

VERSION = "0.5"

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
- When asked to build something, WRITE the .va file with write_file, then RUN it with run_vanta to confirm it works, and fix errors you see.
- For a web app, write it, mention it serves on its port, and tell the user to run `vanta <file>` and open it in Chrome (servers block, so don't run_vanta a serve() program - it will time out; that's expected).
- Keep answers tight. Show the user what you changed and the result."""

# ------------------------------------------------------------------- tools ---
TOOLS = [
    {"name": "read_file", "description": "Read a UTF-8 text file and return its contents.",
     "input_schema": {"type": "object", "properties": {"path": {"type": "string"}}, "required": ["path"]}},
    {"name": "write_file", "description": "Write (create or overwrite) a text file. Use for .va files and any code.",
     "input_schema": {"type": "object", "properties": {"path": {"type": "string"}, "content": {"type": "string"}}, "required": ["path", "content"]}},
    {"name": "list_files", "description": "List files in a directory (defaults to the current directory).",
     "input_schema": {"type": "object", "properties": {"path": {"type": "string"}}}},
    {"name": "run_vanta", "description": "Run a Vanta .va file with the vanta CLI and return its output. Do NOT use on programs that call serve() - they run forever.",
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
    print("  " + dim("write %s (%d lines)" % (p, content.count("\n") + 1)))
    if not _confirm("write"):
        return "User declined to write this file.", "declined"
    d = os.path.dirname(p)
    if d and not os.path.isdir(d): os.makedirs(d)
    with open(p, "w") as f: f.write(content)
    return "Wrote %s" % p, "%d lines" % (content.count("\n") + 1)

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

DISPATCH = {"read_file": tool_read_file, "write_file": tool_write_file,
            "list_files": tool_list_files, "run_vanta": tool_run_vanta, "bash": tool_bash}

def tool_label(name, a):
    if name == "read_file":  return "Read(%s)" % a.get("path", "")
    if name == "write_file": return "Write(%s)" % a.get("path", "")
    if name == "list_files": return "List(%s)" % a.get("path", ".")
    if name == "run_vanta":  return "Run(%s)" % a.get("path", "")
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
    w = term_width()
    print()
    box([
        orange("✻ ") + bold("Welcome to Vanta Code"),
        "",
        dim("  the coding agent that speaks Vanta"),
        "",
        grey("  " + cfg["provider"] + " · " + cfg["model"]),
        grey("  " + os.getcwd()),
    ], w)
    print()
    print(dim("  /help for commands · type a request · Ctrl-C to cancel, Ctrl-D to exit"))
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

# Suggested models per provider for the /model picker (you can also type any name).
MODELS = {
    "anthropic":  ["claude-opus-4-8", "claude-sonnet-4-6", "claude-haiku-4-5-20251001"],
    "openrouter": ["anthropic/claude-sonnet-4.5", "anthropic/claude-opus-4.1", "openai/gpt-4o",
                   "google/gemini-2.5-pro", "deepseek/deepseek-chat", "qwen/qwen3-coder",
                   "meta-llama/llama-3.3-70b-instruct"],
    "ollama":     ["gpt-oss:120b", "gpt-oss:20b", "qwen3-coder:480b", "deepseek-v3.1:671b",
                   "kimi-k2:1t", "glm-4.6"],
}

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
    print(title)
    def draw():
        for i, r in enumerate(rows):
            ptr = orange("❯ ") if i == idx else "  "
            sys.stdout.write("\r\033[K" + ptr + (bold(r) if i == idx else dim(r)) + "\n")
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
                if k == b"A": idx = (idx - 1) % len(rows)
                elif k == b"B": idx = (idx + 1) % len(rows)
                else: continue
            elif b == b"k": idx = (idx - 1) % len(rows)
            elif b == b"j": idx = (idx + 1) % len(rows)
            else: continue
            sys.stdout.write("\033[%dA" % len(rows)); draw()
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
    models = MODELS.get(cfg["provider"], [])
    if not models:
        print(dim("  no preset models for " + cfg["provider"] + " — set one with /model <id>")); return cfg
    idx = models.index(cfg["model"]) if cfg["model"] in models else 0
    sel = select_menu(orange("Choose a model") + dim("   (" + cfg["provider"] + ")  ↑/↓ then Enter"), models, idx)
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
                models = MODELS.get(cfg["provider"], [])
                if not rest:
                    cfg = do_model_menu(cfg)
                elif rest.isdigit() and models:
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
