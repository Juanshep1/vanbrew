# Vanta Code

A terminal coding agent that **speaks Vanta** — styled to feel like Claude Code,
but specialized in the [Vanta](https://github.com/Juanshep1/vanta) language. It
reads, writes, runs, and debugs `.va` programs for you from a chat prompt.

```sh
vanbrew install vcode
export OPENROUTER_API_KEY="sk-or-..."    # or ANTHROPIC_API_KEY / OLLAMA_API_KEY
vcode                                    # run it in any folder; cwd is your project
```

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
- **Real tools** — `read_file`, `write_file`, `list_files`, `run_vanta` (runs a
  `.va` file through the `vanta` CLI and reads the output), and `bash`. Writes
  and shell commands ask for confirmation (`a` = always allow this session, or
  `/auto`).
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

`/help` · `/clear` · `/provider [name]` · `/model [n|name]` · `/auto` · `/cwd <path>` · `/exit`

- **`/provider`** — no argument lists the three providers and which have a key
  set; `/provider ollama` (or `openrouter` / `anthropic`) switches on the spot.
- **`/model`** — a picker: no argument lists models for the current provider,
  numbered; `/model 3` selects the third, or `/model <id>` sets any model.

The command is **`vcode`** — run it in any terminal, or in a folder's terminal to
work on that project (your current directory is the working directory).

## Honest scope

It's a clean, line-based REPL styled to look like Claude Code — same banner,
tool rendering, spinner and flow — not a full raw-mode TUI (no in-box live
editing or syntax-highlighted input). It runs Vanta programs to verify them, but
won't `run_vanta` a `serve()` web app (those run forever) — it'll tell you to
launch that yourself with `vanta <file>` and open it in Chrome.
