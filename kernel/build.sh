#!/bin/sh
# Build the V-NOx kernel: Vanta -> C (vc) -> i386 ELF -> bootable ISO (Limine).
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"; VANTA="$HERE/../../vanta"; LS="$(brew --prefix limine)/share/limine"
CF="--target=i386-elf -ffreestanding -fno-stack-protector -fno-pic -m32 -mno-sse -mno-mmx -O2"
echo "[1/4] vc: kernel.va -> freestanding C"; python3 "$VANTA/vanta.py" "$VANTA/vc.va" "$HERE/kernel.va" -k
echo "[2/4] cross-compile i386"; clang $CF -c "$HERE/boot.s" -o "$HERE/boot.o"
cat "$HERE/kernrt.c" "$HERE/kernel.va.c" > "$HERE/kfull.c"
clang $CF -Wno-implicit-function-declaration -c "$HERE/kfull.c" -o "$HERE/kfull.o"
echo "[3/4] link multiboot ELF"; ld.lld -m elf_i386 -T "$HERE/linker.ld" -o "$HERE/vnox-kernel.elf" "$HERE/boot.o" "$HERE/kfull.o"
echo "[4/4] bootable ISO"; rm -rf "$HERE/isoroot" "$HERE/vnox.iso"; mkdir -p "$HERE/isoroot/EFI/BOOT"
cp "$HERE/vnox-kernel.elf" "$HERE/limine.conf" "$HERE/isoroot/"
cp "$LS/limine-bios.sys" "$LS/limine-bios-cd.bin" "$LS/limine-uefi-cd.bin" "$HERE/isoroot/"
cp "$LS/BOOTX64.EFI" "$LS/BOOTIA32.EFI" "$HERE/isoroot/EFI/BOOT/"
xorriso -as mkisofs -R -r -J -b limine-bios-cd.bin -no-emul-boot -boot-load-size 4 -boot-info-table --efi-boot limine-uefi-cd.bin -efi-boot-part --efi-boot-image --protective-msdos-label "$HERE/isoroot" -o "$HERE/vnox.iso" 2>/dev/null
limine bios-install "$HERE/vnox.iso" >/dev/null 2>&1
echo "done -> vnox.iso"
