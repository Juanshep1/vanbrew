# ⚒ Card Forge

**A bulk card maker, written in [Vanta](https://github.com/Juanshep1/vanta).**
Generate card **artwork** with FLUX (fal.ai / Replicate), lay out **MTG‑style** or
your **own** cards, and export a whole set at once — per‑card PNGs, a print‑and‑play
PDF, and a Tabletop Simulator deck sheet.

```sh
vanbrew install cardforge     # pulls vanta
cardforge                     # opens http://localhost:8120 in a window
```

## What it does
- **100× easier artwork.** Type a card's name + type and it auto‑builds a strong art
  prompt (plus a deck‑wide style you set once), calls **FLUX** via **fal.ai** or
  **Replicate**, and drops the art into the frame. Per‑card prompt override, regenerate,
  and prompt‑hash **caching** so duplicates and re‑renders are free.
- **Two frames.** A clean **MTG‑inspired** frame (mana pips, art window, type line,
  rules/flavor box, rarity, power/toughness — colored by card color) **and** a **custom**
  template for your own game (set the accent/background in Settings).
- **Bulk.** Paste/upload a **CSV** (`name, mana, type, text, flavor, pt, rarity, color,
  art_prompt, template`) → the whole deck fills in. **Generate all art**, then **Render
  all** to high‑res PNGs.
- **Export.** Per‑card **PNG**, a **print‑and‑play PDF** (3×3 poker‑size cards per US
  Letter, with cut guides), and a **Tabletop Simulator** 10‑wide deck sheet.

## Setup
1. Run `cardforge`, open **Settings**.
2. Pick **fal.ai** or **Replicate** and paste an image‑API key.
   - **fal.ai:** model `schnell` (fast/cheap), `dev`, or `pro`.
   - **Replicate:** model `black-forest-labs/flux-schnell` (uses `Prefer: wait`).
3. Set a deck‑wide **art style** (e.g. "dark fantasy oil painting, dramatic light").
4. Click **Test art generation** to confirm your key works.

## How it works (honest notes)
- Pure **Vanta** — it `serve()`s the UI on `:8120` and uses Vanta 4.7's `download`,
  `render_html` (headless **Chrome** rasterizes each card's HTML/CSS to PNG/PDF), and
  binary serving.
- **Desktop (Mac)** for rendering/export — it shells out to Chrome and renders **one
  card at a time** (~1 card/sec, so a 60‑card set takes ~a minute). You can still edit
  and generate art anywhere.
- Frames are MTG‑**inspired** CSS — it does **not** ship Wizards of the Coast's
  copyrighted frame art. The custom template is yours to style freely.
- Art generation uses **your** API key and costs money per image; caching keeps re‑runs
  free.

Files live under `~/.cardforge/` (`state.json`, `art/`, `decks/<name>/`).

## Requirements
- **Vanta 4.7+** (`vanbrew install vanta`) and **Google Chrome**.
- An **image‑API key** (fal.ai or Replicate).

MIT.
