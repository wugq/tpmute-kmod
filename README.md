# tpmute-kmod

An experimental FreeBSD kernel module that turns the ThinkPad embedded
controller's speaker mute into a devd event, so that user programs can wait
for it instead of polling.

Status: **planning, no code yet.** See `PLAN.md` for the steps.

A separate project on purpose: a kernel module has to be rebuilt whenever the
kernel changes, and keeping it apart means only this module needs that, not
[xfce4-bsdthinkpad-plugin](https://github.com/wugq/xfce4-bsdthinkpad-plugin),
which would use its events (and keep polling without it). The user loads it
as root (`kldload tpmute`, or `tpmute_load="YES"` in `/boot/loader.conf`).

## Why

On ThinkPads the speaker mute key is handled by the embedded controller
(EC) in firmware: it mutes the amplifier and changes a bit in the EC, and
that is all. FreeBSD gets no event:

- no ACPI notify reaches devd (checked on a ThinkPad A475 and an X1 Carbon
  3rd generation: `acpi_ibm` events on, the volume/mute bit set in
  `dev.acpi_ibm.0.eventmask`, nothing on `/var/run/devd.pipe` while the
  mute toggled);
- the only way to see a change is to read `sysctl dev.acpi_ibm.0.mute`.

So xfce4-bsdthinkpad-plugin's watcher (`bsdthinkpad`) polls every 200 ms. That costs about 0.15-0.18% of
one CPU (measured with `ps -o time` over 60 s, version 0.3.1), and because
it wakes five times a second it shows up fairly high in `top` among
otherwise idle processes.

A kernel thread polling the same EC bit costs far less than a user process
waking up (no context switch to user space, no syscalls, no GLib main loop).
If it sends a devd event on change, the watcher can block on devd and only
wake when something happens.

## How the mute is read (from `sys/dev/acpi_support/acpi_ibm.c`)

- EC register `IBM_EC_VOLUME` = `0x30`; the speaker mute is bit 6
  (`IBM_EC_MASK_MUTE` = `1 << 6`); the low 4 bits are the EC volume level.
- `acpi_ibm` reads it with `ACPI_EC_READ(sc->ec_dev, IBM_EC_VOLUME, &val, 1)`
  in its sysctl handler (`ACPI_IBM_METHOD_MUTE`), and exports it as
  `dev.acpi_ibm.0.mute`.
- Events from the hotkey device go to devd through
  `acpi_UserNotify("IBM", handle, code)` (codes 0x15-0x17 are volume up,
  volume down, mute; the firmware of both test machines never raises them).

## Design (first idea, to be checked)

- A kernel thread (`kproc`/`kthread`) that loops: read the mute, compare
  with the last value, `pause()` for 200 ms. A thread rather than a
  `callout`, because reading the EC may sleep and callouts must not.
- Read the mute through `kernel_sysctlbyname("dev.acpi_ibm.0.mute", ...)`:
  no dependency on `acpi_ibm`'s private softc layout, so it keeps working
  across FreeBSD versions. (Alternative: find the EC device and use
  `ACPI_EC_READ` directly, as `acpi_ibm` does; more code, more coupling.)
- On a change, `devctl_notify("ACPI", "IBM", "MUTE", "state=0|1")`, which
  devd shows as `!system=ACPI subsystem=IBM type=MUTE state=1`.
- Module depends on `acpi_ibm` (`MODULE_DEPEND`); does nothing and refuses
  to load without `dev.acpi_ibm.0.mute`.
- A sysctl for the interval (`hw.tpmute.interval_ms`, default 200) and one
  to read the last state, for testing.

## Build

A plain kmod, built against the kernel sources in `/usr/src`, from this
directory:

```
make            # bsd.kmod.mk: KMOD=tpmute SRCS=tpmute.c
kldload ./tpmute.ko
```

Packaging: a kmod port of its own, like `graphics/drm-612-kmod` or
`sysutils/acpi_call`.

## Test

```
nc -U /var/run/devd.pipe | grep MUTE    # then press the mute key
```

Measure the cost with the module loaded and the watcher not polling: the
kernel thread's time in `procstat -t` / `top -SH`, against the watcher's
~0.15% today.

## Risks

- A bug in a kernel module panics the machine; test on the A475 (a test
  machine; single-user boot recovers it).
- Built per FreeBSD version (must match the running kernel).
- ThinkPad- and FreeBSD-only.
