# Vanta Code

```
██╗   ██╗ █████╗ ███╗   ██╗████████╗ █████╗ 
██║   ██║██╔══██╗████╗  ██║╚══██╔══╝██╔══██╗
██║   ██║███████║██╔██╗ ██║   ██║   ███████║
╚██╗ ██╔╝██╔══██║██║╚██╗██║   ██║   ██╔══██║
 ╚████╔╝ ██║  ██║██║ ╚████║   ██║   ██║  ██║
  ╚═══╝  ╚═╝  ╚═╝╚═╝  ╚═══╝   ╚═╝   ╚═╝  ╚═╝   c o d e
```

> On startup the wordmark is swept by a "vantablack dusk" gradient (indigo →
> violet → magenta → coral → amber). The official Vanta CLI artwork.


A terminal coding agent that **speaks Vanta** — styled to feel like Claude Code,
but specialized in the [Vanta](https://github.com/Juanshep1/vanta) language. It
reads, writes, runs, and debugs `.va` programs for you from a chat prompt.

```sh
vanbrew install vcode
vcode                                    # first run prompts you to add an API key
```

On first launch vcode walks you through picking a provider and pasting a key
(saved to `~/.vanta-code/config.json`) — no `export` needed. Change it later with
`/provider` or `/key`. You can still pre-set `ANTHROPIC_API_KEY` /
`OPENROUTER_API_KEY` / `OLLAMA_API_KEY` if you prefer.

```
╭──────────────────────────────────────────╮
│ ✻ Welcome to Vanta Code                    │
│                                            │
│   the coding agent that speaks Vanta       │
│   anthropic · claude-sonnet-4-6            │
╰──────────────────────────────────────────╯

› write a fizzbuzz in vanta and run it

⏺ I'll write it and run it to check.
⏺ Write(fizzbuzz.va)
  ⎿  9 lines
⏺ Run(fizzbuzz.va)
  ⎿  exit 0
⏺ Done — it prints 1, 2, Fizz, 4, Buzz ...
```

## What it does

- **Knows Vanta cold** — its system prompt is a compact, accurate reference to
  the whole language (plain-English syntax, `serve`/`http_get`/filesystem
  builtins, the `{{ }}` brace rule, etc.), so the code it writes actually runs.
- **Full computer access** — `read_file`, `write_file`, `edit_file` (surgical
  find/replace), `search` (grep), `glob`, `list_files`, `make_dir`, `move_path`,
  `delete_path`, `run_vanta`, `run_app`, and `bash`. It codes Vanta **anywhere**
  on your machine. Writing is frictionless; deleting/shell ask once (`a` = always).
- **Claude-Code touches** — input **history** (↑/↓), `!shell` to run a command
  directly, `@path` to inline a file, **`/init`** to write a `VANTA.md` that's
  auto-loaded as project context, and **markdown-rendered** replies (bold, code,
  headings, fenced blocks).
- **More Claude-Code touches** — press **Esc** to interrupt the agent mid-task, a
  **context-token meter** above the prompt, **`/resume`** (or `vcode --continue`)
  to reopen your last session, and **`"""`** to send a multi-line message.
- **Looks like Claude Code** — welcome box, `⏺` tool-call lines with `⎿`
  results, a thinking spinner, the bordered prompt, slash commands.

## Bring your own key

Vanta Code thinks with an LLM, so it uses **your** API key from the environment —
nothing is hard-coded and no key is stored:

| Set this | Provider |
| --- | --- |
| `ANTHROPIC_API_KEY` | Claude, directly |
| `OPENROUTER_API_KEY` | OpenRouter (OpenAI-compatible) |
| `OLLAMA_API_KEY` | Ollama Cloud (OpenAI-compatible) |

Set any (or several) and switch live with **`/provider`**. Optional:
`VANTA_CODE_MODEL=<model>` to pick a specific model.

## Commands

`/help` · `/clear` · `/compact` · `/themes` · `/provider [name]` · `/model [n|name]` · `/auto` · `/cwd <path>` · `/exit`

- **`/provider`** — an **arrow-key menu** (↑/↓, Enter, Esc to cancel). Pick a
  provider; if it has no key yet you're prompted to **paste your API key**, which
  is saved to `~/.vanta-code/config.json` (hidden input, file locked to `600`),
  then it flows straight into the model picker. `/provider ollama` still switches
  directly. Your choice + key + model persist across sessions.
- **`/model`** — an arrow-key menu of the provider's **full live model list**
  (`/v1/models`): Ollama Cloud's whole catalog and **all ~337 OpenRouter models**.
  Big lists scroll, and you can **type to filter** (e.g. type `claude` to narrow).
  `/model <id>` sets any model, `/model refresh` re-pulls the list.

The command is **`vcode`** — run it in any terminal, or in a folder's terminal to
work on that project (your current directory is the working directory).

## Running projects

Tell it to **run / open / launch / show** a project and it actually launches it —
the `run_app` tool pops the app up in a **movable, draggable window**. A web app
or the tip calculator opens chromeless and movable; a `serve()` app is started
and opened at its port. (Plain non-visual scripts still run through `run_vanta`
for their console output.)

## Themes & diffs

- **`/themes`** — pick a color theme (ember · synthwave · matrix · ice · gold ·
  mono). It restyles the accent and the gradient VANTA wordmark, and is saved.
- **Live diffs** — when it writes or edits a file you see a Claude-Code-style
  diff: `+` lines in green, `-` lines in red, with line numbers and context.

## Honest scope

It's a clean, line-based REPL styled to look like Claude Code — same banner,
tool rendering, spinner and flow — not a full raw-mode TUI (no in-box live
editing or syntax-highlighted input). It runs Vanta programs to verify them and
launches visual/web projects in a movable window via `run_app`.
