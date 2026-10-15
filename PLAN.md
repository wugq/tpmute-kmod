# Plan

Each step can be checked on its own before the next one starts.

## 1. Read the source

- `sys/dev/acpi_support/acpi_ibm.c`: the sysctl handler for
  `dev.acpi_ibm.0.mute`, its locking (`ACPI_SERIAL_BEGIN(ibm)`), whether a
  read can sleep, and how `acpi_UserNotify()` builds its devd message.
- `kernel_sysctlbyname(9)`-style reading from a kernel thread: is it safe
  to call the `acpi_ibm` handler from there (Giant, sysctl locks)?
- `devctl_notify(9)`, `kproc_create(9)` / `kthread_add(9)`, `pause(9)`,
  module load/unload (`DECLARE_MODULE` / `moduledata_t`, stopping the thread
  cleanly on `MOD_UNLOAD`).
- Decide: sysctl read (decoupled) or direct `ACPI_EC_READ` (as acpi_ibm).

## 2. Write the module (~100-150 lines)

- `tpmute.c`, `Makefile` (`KMOD= tpmute`, `SRCS= tpmute.c`,
  `.include <bsd.kmod.mk>`).
- Load: check `dev.acpi_ibm.0.mute` exists, start the thread.
- Thread: read, compare, `devctl_notify` on change, `pause("tpmute", hz/5)`;
  exit when asked to on unload.
- Sysctls: `hw.tpmute.interval_ms`, `hw.tpmute.state` (read only).
- Build with `-Wall` clean on FreeBSD 15.1.

## 3. Test on the A475

- `kldload`, `kldunload`, load again: no leak, no panic, thread gone after
  unload (`procstat -t 0` / `top -SH`).
- `nc -U /var/run/devd.pipe` (not `devd.seqpacket.pipe`: `nc -U` can not
  talk to a SOCK_SEQPACKET socket): one event per press of the mute key,
  with the right state; also when set by `sysctl dev.acpi_ibm.0.mute=1`.
- Cost: the thread's CPU time over a few minutes.

## Results (2026-10-14)

- Step 1: the mute sysctl is `CTLFLAG_MPSAFE` (no Giant), serialised by an
  sx lock (`ACPI_SERIAL_*`), so a kernel thread can read it through
  `kernel_sysctl()`. Chosen: sysctl read. `acpi_ibm` has no
  `MODULE_VERSION`, so no `MODULE_DEPEND`.
- Step 2: `tpmute.c`, ~270 lines, `-Wall -Werror` clean.
- Step 3: one event per change, right state, on both machines; load,
  unload, reload clean; load without `acpi_ibm` fails with ENXIO;
  `acpi_ibm` unloaded and reloaded under tpmute keeps working.
  Bug found and fixed: after a failed `MOD_LOAD` the linker sends
  `MOD_UNLOAD`, which slept forever waiting for a thread that never
  started (hung the A475's kldload; needed a reboot).
- Cost on the X1 Carbon: 0.036 s system time in 180 s (~0.02% of a CPU) at
  200 ms, against ~0.15% for bsdthinkpad 0.3.1's poll.
- Still to do: press the physical mute key (only sysctl writes tested);
  boot with `tpmute_load="YES"`.

## 4. Decide and integrate

Only if the gain is real.

### The interface between the two projects

The devd event is the only contract between tpmute-kmod and
xfce4-bsdthinkpad-plugin. Fix it in README.md before the plugin uses it, and
do not change it afterwards without changing both:

```
!system=ACPI subsystem=IBM type=MUTE state=0|1
```

plus a way to tell the module is loaded: the sysctl node `hw.tpmute`
(e.g. `hw.tpmute.state`, `hw.tpmute.interval_ms`). If the format ever has
to change, add a version sysctl (`hw.tpmute.version`) and let the plugin
check it.

### What the user does

1. `pkg install tpmute-kmod` (or build its port).
2. `kldload tpmute` now; `sysrc -f /boot/loader.conf tpmute_load=YES` for
   every boot.

Nothing to set in bsdthinkpad, no restart: it notices the module itself.
Without the module everything works as today.

### What bsdthinkpad does (changes in `bsdthinkpad.c`)

- Detect: at start, and again every few seconds while not connected, check
  whether `hw.tpmute` exists (`sysctlbyname`).
- Module loaded: connect to `/var/run/devd.seqpacket.pipe` (SOCK_SEQPACKET,
  one event per read), watch it from the GLib main loop
  (`g_unix_fd_add`); on `system=ACPI subsystem=IBM type=MUTE`, read
  `dev.acpi_ibm.0.mute` and do what a poll does today on a change (mute LED,
  PulseAudio sink sync). Stop the 200 ms poll of the speaker mute; keep a
  slow safety poll (~2 s) so a lost event is caught.
- Module not loaded, unloaded later, or the devd socket closes (devd
  restarted): back to the 200 ms poll, and try to reconnect.
- Brightness and microphone could move to events too (its BACKLOG,
  "Fewer wake-ups"): the mic-mute key already has a devd notify (0x1b),
  brightness could be signalled by `bsdthinkpad brightness`; a slow poll
  stays for changes made by other tools.
- Its README gets a short "Optional: tpmute-kmod" section pointing here.

### Packaging

A kmod port of its own, like `sysutils/acpi_call` or
`graphics/drm-612-kmod`; after a kernel update only this package is rebuilt.

## Notes from earlier work

- Measured 2026-10-14: bsdthinkpad 0.3.1 uses 0.11 s / 60 s on the A475
  (~0.18%) and 0.09 s / 60 s on the X1 Carbon (~0.15%).
- In 0.3.1 one poll costs ~110 us on the A475: EC mute ~26-32 us, mic mute
  ~76 us (2 ioctls per mixer), brightness ~4-8 us. The rest of the CPU time
  is the cost of waking up.
- Neither machine's firmware raises the ACPI notify 0x15-0x17 for the
  volume/mute keys, even though `acpi_ibm` would pass it to devd.
