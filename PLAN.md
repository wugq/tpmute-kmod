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

## 4. Decide and integrate

Only if the gain is real:
- xfce4-bsdthinkpad-plugin (`bsdthinkpad.c`): read `/var/run/devd.pipe`
  (or devd.seqpacket.pipe from C) for the speaker mute; fall back to
  polling when tpmute is not loaded.
- Brightness and microphone could move to events too (its BACKLOG,
  "Fewer wake-ups"), leaving a slow fallback poll.
- Packaging: a kmod port of its own (`tpmute_load="YES"` in loader.conf).

## Notes from earlier work

- Measured 2026-10-14: bsdthinkpad 0.3.1 uses 0.11 s / 60 s on the A475
  (~0.18%) and 0.09 s / 60 s on the X1 Carbon (~0.15%).
- In 0.3.1 one poll costs ~110 us on the A475: EC mute ~26-32 us, mic mute
  ~76 us (2 ioctls per mixer), brightness ~4-8 us. The rest of the CPU time
  is the cost of waking up.
- Neither machine's firmware raises the ACPI notify 0x15-0x17 for the
  volume/mute keys, even though `acpi_ibm` would pass it to devd.
