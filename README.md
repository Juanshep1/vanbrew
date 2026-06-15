# Vanbrew

**pip + Homebrew, in one file.** A tiny, universal package manager that installs
[Vanta](packages/vanta) and anything else you write a small recipe for.

```sh
curl -fsSL https://raw.githubusercontent.com/Juanshep1/vanbrew/main/install.sh | sh
```

Then:

```sh
vanbrew install vanta      # now `vanta file.va` works anywhere
vanbrew install vnox       # then run `vnox` to boot the V-NOx desktop OS
vanbrew install topdeck    # now just run `topdeck`
vanbrew search .           # browse the whole catalog
```

## Why

A package manager that installs a brand-new language can't be *written* in that
language — it has to run before the language exists. So Vanbrew leans on the one
runtime that's already on basically every computer: **Python**. The whole thing
is a single ~370-line script with **zero third-party dependencies**, written to
run on **Python 2.7 or 3.x**, on **macOS / Linux / Windows**, old machines and new.

## Commands

| Command | Does |
| --- | --- |
| `vanbrew install <pkg>...` | install packages (resolves dependencies) |
| `vanbrew uninstall <pkg>...` | remove packages |
| `vanbrew list` | what's installed |
| `vanbrew search <q>` | search the catalog (`.` = everything) |
| `vanbrew info <pkg>` | details for a package |
| `vanbrew update` | pull the latest catalog from GitHub |
| `vanbrew doctor` | check your setup |

Everything lives under `~/.vanbrew` (`bin/` shims, `cellar/` versioned installs,
`registry/` extra recipes). Uninstalling is just deleting that folder.

## The catalog

| Package | What |
| --- | --- |
| `vanta` | the Vanta plain-English language + interpreter (**4.4** — a full web stack: `serve`, an HTTP client with gzip/charset decoding, filesystem, JSON, and `run_vanta` for running Vanta in-process) |
| `vnox` | **V-NOx 1 — the first OS written in Vanta.** A whole desktop environment served by one Vanta program on port 8100 — window manager, virtual filesystem, Vanta Studio, Files, Terminal, Settings, Calculator, and a real **web browser** (searches and reads live sites through a Vanta proxy, multi-tab with back/forward, and opens logins/video in Firefox). |
| `vself` | a Vanta interpreter **written in Vanta** — self-hosting; runs lists/maps/recursion and even its own source |
| `vc` | a self-hosting **Vanta→C compiler** — compiles Vanta (even V-NOx, and itself) to native, Python-free binaries, with a garbage collector |
| `topdeck` | Yu-Gi-Oh! Master Duel meta analyzer (Vanta web app, port 8090) |
| `vaeldric` | Vaeldric conlang site + JSON API (Vanta web app, port 8080) |
| `tipjar` | a draggable tip calculator that pops up in your browser — a fun little demo |
| `hello` | a tiny demo package |

## Bonus: V-NOx on bare metal

The [`kernel/`](kernel) folder is a different beast: a **bootable graphical
desktop OS** with no operating system, no Python, and no libc beneath it. Vanta
source is compiled to C by `vc`, linked freestanding into a 32-bit kernel, and
booted by Limine in QEMU. It paints an "aurora over obsidian" desktop into a raw
framebuffer and runs a real window manager — draggable windows with close
buttons, a clickable dock that opens apps, a PS/2 mouse cursor, and a
keyboard-driven terminal — all written in Vanta.

```sh
cd kernel && ./build.sh && ./run.sh    # needs clang, ld.lld, limine, qemu
```

## Add your own package

A "formula" is just JSON. Two ways to add one:

1. **Locally** — drop a `.json` file into `~/.vanbrew/registry/`:

   ```json
   {
     "name": "mytool",
     "version": "1.0",
     "summary": "what it does",
     "deps": ["vanta"],
     "files": [
       {"source": {"kind": "url", "url": "https://.../mytool.va"}, "as": "mytool.va"}
     ],
     "bin": [{"name": "mytool", "kind": "vanta", "main": "mytool.va"}]
   }
   ```

2. **For everyone** — open a PR adding your formula to [`registry.json`](registry.json).

**Source kinds:** `url` (download, optional `sha256`), `local` (copy a path),
`inline` (embed the text).
**Bin kinds:** `python` (run with python), `vanta` (run with vanta), `shell`
(run with sh), `exec` (the file is itself executable).

## Notes

* Requires Python (2.7+/3.x) and, for downloads, internet + `curl`/`wget`.
* The installer adds `~/.vanbrew/bin` to your `PATH` via a clearly-marked block
  in your shell profile (`# >>> vanbrew >>>`). Remove it to undo.
* Fork it: set `VANBREW_RAW` to your own raw GitHub base and the catalog points
  at your fork.
