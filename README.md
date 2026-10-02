# Aporia OS v1.0.0 Workstation

A standalone 32-bit x86 operating system featuring preemptive multitasking, hardware PCI bus enumeration, an ext2 virtual filesystem, and a desktop compositor running in VESA VBE high-resolution mode.

## Architectural Specifications

* **CPU Target:** x86 Protected Mode (Ring 0 Kernel, Ring 3 Userspace isolation).
* **Display Engine:** VESA VBE 1024x768 @ 32 bpp linear framebuffer with hardware-composited windows and damage-rectangle acceleration.
* **Storage & VFS:** ATA PIO hard disk subsystem mounting an **ext2** filesystem volume; direct ELF32 dynamic binary execution.
* **Hardware Bus:** PCI Configuration Space scanning and class enumeration (`lspci`).
* **Kernel Core:**
  * Two-level MMU paging with isolated user process frames.
  * Task State Segment (TSS) and interrupt gating (`int 0x80` syscall dispatcher).
  * Round-Robin preemptive scheduler (PIT 100 Hz).
  * PS/2 mouse driver with title bar drag-and-drop window management.

## Project Structure

```text
├── Makefile             # Unified build pipeline
├── src
│   ├── boot             # Real Mode bootstrap & protected mode switch
│   │   └── boot.asm
│   ├── kernel           # Monolithic kernel core, drivers & VFS
│   │   ├── isr.asm
│   │   ├── kernel.c
│   │   └── kernel_entry.asm
│   └── user             # Ring 3 userspace binaries & linker script
│       ├── app.asm
│       ├── user_elf.c
│       └── user_elf.ld
└── tools
    └── injector.c       # ext2 volume injection tool

Building and Running
Prerequisites

    GCC (gcc, 32-bit multilib support)

    NASM (nasm)

    GNU Linker (ld)

    QEMU (qemu-system-i386)

Compilation
Bash

make clean
make

Launching under QEMU
Bash

make run

