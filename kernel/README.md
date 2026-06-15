# V-NOx — a graphical desktop with draggable windows, on a Vanta kernel

The first **interactive graphical desktop running on a bare-metal kernel written
in Vanta** — no OS underneath, no Python, no libc. Vanta code paints a desktop
into a linear framebuffer and runs a real window manager: **draggable windows**
with **close buttons**, a **clickable dock that opens apps** (Studio, Files,
About), a taskbar, a PS/2 mouse cursor, and a keyboard-driven terminal.

```sh
./build.sh        # Vanta -> C (vc) -> i386 ELF -> bootable ISO (Limine)
./run.sh          # boot in QEMU - drag the windows, type in the terminal
```

## How it works
- `kernel.va` — the desktop + **window manager, in Vanta**: holds a list of window
  maps, polls the mouse each frame, and on a fresh click decides what was hit —
  a **dock icon** (opens that app), a window's **X** (closes it), or a **title
  bar** (starts a drag). Drags the grabbed window, brings it to the front, and
  redraws (double-buffered).
- `kernrt.c` — a **freestanding** runtime: Value system + framebuffer blitter +
  double buffer + 8x8 font + **PS/2 mouse driver** + polled keyboard + a per-frame
  bump-GC. Exposes `fill/text_at/rgb/cursor/clear/present/poll/mouse_x/mouse_y/
  mouse_down/key/gc_mark/frame_reset` to Vanta.
- `boot.s` requests a 1024x768x32 framebuffer (multiboot); Limine provides it.
- `vc -k` emits `kmain()` with no libc (the kernel runtime supplies everything).

## Honest scope
A genuine interactive desktop on bare metal: draggable windows with close
buttons, a clickable dock that opens apps, a mouse cursor, a taskbar, on-screen
code, and a working terminal. It is not the full
web-V-NOx (no networking, no real apps with logic, no file system on disk) — those
need more drivers — but the windowing desktop boots on bare metal and you drive
it with mouse + keyboard.

## Booting
- **QEMU** (proven): `qemu-system-i386 -cdrom vnox.iso -vga std`.
- Real x86 PCs / x86 VirtualBox hosts: the ISO is BIOS+UEFI bootable.
- Apple-Silicon Macs can't run x86 in VirtualBox — use QEMU.
