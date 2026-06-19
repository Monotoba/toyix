# Toyix

[![CI](https://github.com/Monotoba/toyix/actions/workflows/ci.yml/badge.svg)](https://github.com/Monotoba/toyix/actions/workflows/ci.yml)
[![Release](https://github.com/Monotoba/toyix/actions/workflows/release.yml/badge.svg)](https://github.com/Monotoba/toyix/actions/workflows/release.yml)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)
[![Platform](https://img.shields.io/badge/platform-i686%20x86-lightgrey.svg)](Makefile)
[![Tests](https://img.shields.io/badge/test%20coverage-boot%20smoke-blue.svg)](tests/smoke.sh)

Toyix is a small Linux-style teaching operating system written in C and x86 assembly. It currently boots as a Multiboot kernel through GRUB, initializes serial and VGA text consoles, and verifies the boot log through an automated QEMU smoke test.

<p>
  <img src="docs/assets/toyix-preview.png" alt="Toyix preview" width="360">
</p>

## Current Features

- Multiboot-compatible i686 kernel entry point
- Freestanding C kernel build
- Linker script placing the kernel at 1 MiB
- Serial console driver for automated test capture
- VGA text console driver for emulator output
- QEMU smoke test that checks the boot log
- GitHub Actions CI for build and smoke test validation

## Repository Layout

```text
arch/                 Architecture-specific boot code
drivers/              Hardware-facing drivers
include/              Public kernel headers
kernel/               Core kernel code and small freestanding helpers
tests/                Smoke test scripts
articles/             Tutorial chapters
docs/                 Project documentation and assets
```

## Requirements

The build expects an i686 ELF cross-compiler toolchain and common OS development tools:

- `i686-elf-gcc`
- `nasm`
- `grub-file`
- `grub-mkrescue`
- `qemu-system-i386`

The CI workflow builds the cross-compiler toolchain before running the project tests.

## Build

```sh
make
```

## Build an ISO

```sh
make iso
```

The bootable image is written to `build/toyix.iso`.

## Run

```sh
make run
```

## Test

```sh
make test
```

or:

```sh
tests/smoke.sh
```

The smoke test builds the ISO, boots it under QEMU, captures serial output, and verifies the expected kernel messages.

## Documentation

- [Series introduction](index.md)
- [Chapter 1](articles/chapter_01.md)
- [Roadmap](docs/roadmap.md)

## License

Toyix is released under the MIT License. See [LICENSE](LICENSE).

