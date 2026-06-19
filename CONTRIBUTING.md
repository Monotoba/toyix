# Contributing

Toyix is a small teaching operating system, so changes should favor clarity, small steps, and testable behavior.

## Development

1. Install the required i686 toolchain, GRUB utilities, NASM, and QEMU.
2. Run `make test` before submitting changes.
3. Keep generated files out of commits. Build outputs belong under `build/`.

## Style

- Keep kernel code freestanding and avoid hosted C library assumptions.
- Prefer narrow interfaces between kernel core code and hardware-specific drivers.
- Add or update documentation when behavior changes in a way readers need to understand.

