# Repository Guidelines

## Project Structure & Module Organization

This repository packages an out-of-tree Linux hwmon driver for ITE IT5570 systems. Driver source lives in `drv/`: `it5570_main.c` handles module parameters, probing, and transport selection; `it5570_core.c` contains shared SuperIO/D2EC access and diagnostics; `it5570_sensors.c` registers hwmon/sysfs nodes and conversion logic; `it5570_hwmon.h` holds shared definitions. Hardware reference material is in `docs/`, including the IT5570 manual Markdown. App-store packaging files, icons, lifecycle scripts, and metadata are under `package/`. There is currently no dedicated test directory.

## Build, Test, and Development Commands

Build on a target Linux host with matching kernel headers:

```sh
make -C drv
make -C drv TARGET="$(uname -r)"
make -C drv clean
```

Inspect generated module metadata:

```sh
modinfo drv/it5570_hwmon.ko
modinfo -F revision drv/it5570_hwmon.ko
modinfo -F vermagic drv/it5570_hwmon.ko
```

Load and unload for manual validation:

```sh
sudo insmod drv/it5570_hwmon.ko transport=d2ec
dmesg | tail -n 120
sudo rmmod it5570_hwmon
```

Do not claim build verification from Windows-only environments; this module requires a Linux kernel build tree.

## Coding Style & Naming Conventions

Follow Linux kernel C style: tabs for indentation, lowercase identifiers, and focused helper functions. Keep module parameters, transport names, sysfs attributes, and log keys stable unless behavior intentionally changes. Prefer explicit hardware semantics over generic abstractions. When adding register conversions, cite the relevant formula or register behavior near the code.

## Testing Guidelines

No automated unit test framework is present. Validate changes by building the module, checking `modinfo`, loading on matching hardware, reviewing `dmesg`, and reading `/sys/class/hwmon/...` nodes. Test negative paths with documented parameters such as `fault_inject=...` where relevant. Confirm `pwmN` and raw debug nodes remain read-only unless write support is explicitly redesigned.

## Commit & Pull Request Guidelines

Recent commits use Conventional Commit-style subjects such as `fix(hwmon): ...`, `ci(package): ...`, and `chore(package): ...`. Keep subjects scoped and imperative. Pull requests should describe driver behavior changes, target kernels tested, build/load results, relevant `dmesg` excerpts, and any packaging updates. Update `drv/README.md` when module parameters, exposed nodes, transport behavior, or validation steps change.

## Hardware & Safety Notes

The authoritative sensor path is currently `d2ec`. Do not fabricate readings for unsupported transports such as `smfi`, `pmc*`, or `peci`; return errors or skip registration instead. Treat PWM and raw debug sysfs files as observation interfaces, not control APIs.
