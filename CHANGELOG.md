# Changelog

All notable changes to the Aporia OS project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [1.0.1] - 2026-10-03

### Fixed
- Corrupted prerequisite parsing and recipe formatting in `Makefile` (`buildnasm` anomaly).
- Broken argument separation during disk image injection step.
- Continuous Integration workflow execution on GitHub Actions.

### Removed
- Untracked workspace metadata and `.vscode` configuration files from version control.

## [1.0.0] - 2026-10-02

### Added
- Protected Mode x86 Monolithic Kernel baseline.
- Preemptive Round-Robin multitasking scheduler (PIT 100 Hz).
- Ring 3 user process isolation with Task State Segment (TSS) and paging.
- Hardware PCI Configuration Space scanner and class decoder (`lspci`).
- Native ext2 Virtual Filesystem driver with ELF32 binary loader (`fs_exec`).
- VESA VBE 1024x768x32bpp graphical composer with hardware-composited windows.
- PS/2 mouse subsystem with non-flickering damage-rectangle window dragging.
