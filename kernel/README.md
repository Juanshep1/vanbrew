# V-NOx — a graphical desktop on a kernel written in Vanta

The first **graphical OS desktop running on a bare-metal kernel, drawn by Vanta** —
no operating system underneath, no Python, no libc. Vanta code paints a desktop
(wallpaper, top bar, dock, windows, taskbar, a live terminal) straight into a
linear framebuffer, and a polled PS/2 keyboard driver makes it interactive.

```sh
./build.sh                 # Vanta -> C (vc) -> i386 ELF -> bootable ISO (Limine)
./run.sh                   # boot it in QEMU - click the window and type
```

## How it works
- `kernel.va` — the desktop, **in Vanta**: calls `wallpaper()`, `fill(x,y,w,h,color)`,
  `text_at()/text_big()`, `rgb()`, `screen_w/h()`, and `key()` in an event loop.
- `kernrt.c` — a **freestanding** runtime: the Value system + a framebuffer
  blitter + an embedded 8x8 font + a polled PS/2 keyboard. No libc, no syscalls.
- `boot.s` — multiboot1 stub that **requests a 1024x768x32 framebuffer**; Limine
  sets it up and passes it in.
- `vc -k` — emits `kmain()` with no libc runtime (the kernel runtime supplies it).

## Honest scope
A real, interactive **framebuffer desktop**: windows, a dock, a taskbar, code on
screen, and a keyboard-driven terminal — all from Vanta on bare metal. It is not
yet the full web-V-NOx (no mouse/window-dragging, no networking, no real apps);
those need a mouse driver, a window manager, and device drivers — the natural
next steps. But the desktop boots on bare metal and you can type into it.

## Booting
- **QEMU** (proven, graphical): `qemu-system-i386 -cdrom vnox.iso -vga std`.
- Real x86 PCs / x86 VirtualBox hosts: the ISO is BIOS+UEFI bootable.
- Apple-Silicon Macs can't run x86 in VirtualBox — use QEMU (works great).
