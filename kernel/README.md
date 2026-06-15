# V-NOx Kernel — the first kernel written in Vanta

A real **bare-metal kernel**: no operating system underneath, no Python, no
runtime. The kernel logic is written in **Vanta**, compiled to C by `vc` (the
Vanta-to-C compiler, itself written in Vanta), cross-compiled to i386 machine
code, and booted via the multiboot protocol. It prints to VGA text mode and the
serial port.

```sh
./build.sh                         # Vanta -> C -> i386 ELF -> vnox.iso
qemu-system-i386 -kernel vnox-kernel.elf -serial stdio   # boot the ELF directly
./run.sh                           # or boot the ISO in a QEMU window
```

## Pieces
- `kernel.va`   — the kernel, in Vanta (what it computes/prints on boot).
- `kernrt.c`    — a **freestanding** Value runtime (bump allocator + VGA/serial
  output + string ops); no libc, no syscalls.
- `boot.s`      — multiboot1 boot stub (sets the stack, calls `kstart`).
- `linker.ld`   — loads the kernel at 1 MiB.
- `vc -k`       — emits `kmain()` with no libc runtime (the kernel runtime supplies it).

## Honest scope
This is a **text-mode** kernel — it boots, runs Vanta (arithmetic, strings,
lists, maps, loops, recursion) and prints to the screen. It is NOT the V-NOx
*desktop* (that needs graphics/USB/network drivers, far beyond this). It's the
genuine "hello, bare metal" milestone: Vanta running with nothing underneath it
but the CPU.

## Booting elsewhere
- **QEMU** (proven): `qemu-system-i386 -cdrom vnox.iso`.
- **Any x86 PC / x86 VirtualBox host**: the ISO is BIOS+UEFI bootable.
- **VirtualBox on Apple-Silicon Macs**: can't run x86 guests — use QEMU instead.
