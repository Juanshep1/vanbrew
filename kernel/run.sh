#!/bin/sh
cd "$(dirname "$0")"
qemu-system-i386 -cdrom vnox.iso -m 256
