#!/bin/sh
# Vanbrew installer. Downloads Vanbrew and runs first-time setup.
#   curl -fsSL https://raw.githubusercontent.com/Juanshep1/vanbrew/main/install.sh | sh
set -e

BASE="${VANBREW_RAW:-https://raw.githubusercontent.com/Juanshep1/vanbrew/main}"

printf '\n  Installing Vanbrew (pip + Homebrew, in one file)...\n\n'

# pick a downloader
if command -v curl >/dev/null 2>&1; then
  GET() { curl -fsSL "$1" -o "$2"; }
elif command -v wget >/dev/null 2>&1; then
  GET() { wget -qO "$2" "$1"; }
else
  echo "error: need curl or wget to install" >&2
  exit 1
fi

# pick a python
PY=python3
command -v python3 >/dev/null 2>&1 || PY=python
command -v "$PY" >/dev/null 2>&1 || { echo "error: Python is required" >&2; exit 1; }

TMP="$(mktemp 2>/dev/null || echo "/tmp/vanbrew.$$")"
GET "${BASE}/vanbrew.py" "$TMP"

"$PY" "$TMP" setup
rm -f "$TMP"
