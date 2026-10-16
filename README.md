# tpmute-kmod

An experimental FreeBSD kernel module that turns the ThinkPad embedded
controller's speaker mute into a devd event, so that user programs can wait
for it instead of polling.

Status: **experimental.** The module works (tested on FreeBSD 15.1, A475 and
X1 Carbon 3rd gen); see `PLAN.md` for the steps and results.

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

## Design

- A kernel thread (`kproc`/`kthread`) that loops: read the mute, compare
  with the last value, `pause()` for 200 ms. A thread rather than a
  `callout`, because reading the EC may sleep and callouts must not.
- Read the mute through `kernel_sysctlbyname("dev.acpi_ibm.0.mute", ...)`:
  no dependency on `acpi_ibm`'s private softc layout, so it keeps working
  across FreeBSD versions. (Alternative: find the EC device and use
  `ACPI_EC_READ` directly, as `acpi_ibm` does; more code, more coupling.)
- On a change, `devctl_notify("ACPI", "IBM", "MUTE", "state=0|1")`, which
  devd shows as `!system=ACPI subsystem=IBM type=MUTE state=1`.
- No `MODULE_DEPEND` on `acpi_ibm`: it has no `MODULE_VERSION`, so the
  dependency could never be satisfied. Instead the module refuses to load
  without `dev.acpi_ibm.0.mute`, except at boot (loader.conf), when
  `acpi_ibm` has not attached yet; the thread retries. If `acpi_ibm` is
  unloaded and loaded again, the OID is looked up again.
- A sysctl for the interval (`hw.tpmute.interval_ms`, default 200) and one
  to read the last state, for testing.
- On the ThinkPad A475 only, it also sets the speaker mute LED from the
  EC's mute on every poll (`hw.tpmute.led`: 1 on, 0 off, -1 not this
  model; also a loader tunable). The firmware sets that LED only from its
  own copy of the mute (`AUDC`, behind `SSMS`/`GSMS`), which lags the EC by
  130 to 180 ms. The LED is AMD FCH GPIO 143 (DSDT `\_SB.PCI0.SMB.G143`):
  bit 22 of the register at `0xFED81500 + 0x23C`, low lights it. The module
  maps that register only when `smbios.system.version` is "ThinkPad A475"
  and writes only that bit, only while the pin is an output (bit 23).
- Keep the interval at 200 ms: every read is an EC transaction, and while
  the key is pressed quickly reads collide with the EC's own work
  (`acpi_ec0: EcCommand: no response to 0x84`, presses lost). Measured on
  the A475: none with nothing reading, a few at 200 ms, many when the
  watcher read every 30 ms as well (so it now reads `hw.tpmute.state`).

## Build

A plain kmod, built against the kernel sources in `/usr/src`, from this
directory:

```
make            # bsd.kmod.mk: KMOD=tpmute SRCS=tpmute.c
kldload ./tpmute.ko
```

### As a package (port)

The port is in `port/sysutils/tpmute-kmod`; it fetches the release (tag
`v0.1.0`) from GitHub. It is not in the FreeBSD ports tree; a port directory
works from anywhere, given the ports framework (`/usr/ports/Mk`) and the
kernel sources (`/usr/src`). As root:
```
git clone https://github.com/wugq/tpmute-kmod
cd tpmute-kmod/port/sysutils/tpmute-kmod
make package        # work/pkg/tpmute-kmod-0.1.0.<FreeBSD version>.pkg
pkg install work/pkg/tpmute-kmod-*.pkg
make clean
```
The package installs `/boot/modules/tpmute.ko`, built for the kernel in
`/usr/src`; rebuild it after a FreeBSD upgrade.

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
