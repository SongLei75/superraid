# Contributing to SuperRAID

SuperRAID is an experimental embedded Linux storage-reliability project maintained by [@SongLei75](https://github.com/SongLei75). Bug reports, test results, documentation improvements, and focused fixes are welcome.

## Before opening an issue

- Check existing issues and the [development TODO](TODO.md).
- Describe the expected and actual behavior, relevant kernel/build environment, and steps to reproduce.
- Include relevant logs or test results, but remove credentials, private device identifiers, and proprietary information.

## Before submitting a pull request

- Open an issue first for significant changes or design proposals.
- Keep patches minimal and focused. Explain the reason for the change and the test environment used.
- For changes affecting FAT/VFAT, startup recovery, or shutdown, document the effect on data consistency and possible failure paths.
- Preserve existing copyright and SPDX license identifiers in Linux-derived source files. Only submit code or material you have permission to contribute.

The project is not production-ready. Maintainers may request additional validation before accepting changes.

## Scope

The existing [technical design](README.md) is the current target architecture. The [standalone implementation](power-ctl/) and [development TODO](TODO.md) reflect the current state, including known limitations.
