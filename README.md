# Vanbrew

**pip + Homebrew, in one file.** A tiny, universal package manager that installs
[Vanta](packages/vanta) and anything else you write a small recipe for.

```sh
curl -fsSL https://raw.githubusercontent.com/Juanshep1/vanbrew/main/install.sh | sh
```

Then:

```sh
vanbrew install vanta      # now `vanta file.va` works anywhere
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
| `vanta` | the Vanta plain-English language + interpreter |
| `vnox` | **V-NOx 1 — the first OS written in Vanta** (desktop environment, port 8100) |
| `topdeck` | Yu-Gi-Oh! Master Duel meta analyzer (Vanta web app, port 8090) |
| `vaeldric` | Vaeldric conlang site + JSON API (Vanta web app, port 8080) |
| `hello` | a tiny demo package |

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
