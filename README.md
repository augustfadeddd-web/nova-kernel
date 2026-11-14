# Nova Kernel

The x86-64 Multiboot2 kernel from the NovaOS integrated project, written in C and assembly.

Includes boot and long-mode entry, physical pages, user processes and syscall support, graphics, input, RTL8139 networking, ATA and FAT32 storage. The existing kernel initializes a graphical login and desktop, so their linked source and embedded graphics assets are included.

## Build

On Linux, install GCC, GNU binutils, Make and Python 3. On macOS, install the Xcode command line tools, LLVM (providing `ld.lld` on PATH), Make and Python 3.

```sh
make
```

Output: `build/kernel.elf`. The build checks its Multiboot2 header. Boot using a Multiboot2-compatible loader; `grub.cfg` is provided. Nova requires a 24/32-bit RGB framebuffer and a legacy BIOS boot setup. Demo login: `nova` / `nova`.

This source package excludes the full OS distribution, SDK applications and tools, host browser helper, VM images, prebuilt OS binaries, and external applications. It does not package a bootable ISO.

The shared syscall ABI header is retained in `sdk/nova.h` because the kernel includes it.

## Notices

See [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) and [licenses/DejaVu.txt](licenses/DejaVu.txt). No new license is assigned to the original Nova code by this publication.
