#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Loads ceesafe_mq.ko, exercises /dev/ceesafe_mq from user space, unloads it with messages still queued, and checks
the kernel log. Run as root in the module's build directory, on a machine you can afford to crash (a VM)."""
import ctypes
import errno
import os
import subprocess
import sys

DEVICE = "/dev/ceesafe_mq"
SLOTS = 16
MESSAGE_MAX = 512
failures = []


def check(name, condition, detail=""):
    print(("PASS " if condition else "FAIL ") + name + (f": {detail}" if detail and not condition else ""))
    if not condition:
        failures.append(name)


def errno_of(action):
    try:
        action()
    except OSError as error:
        return error.errno
    return 0


def kernel_log():
    return subprocess.run(["dmesg"], capture_output=True, text=True, check=True).stdout


def kernel_has(option):
    """Whether the running kernel was built with CONFIG_<option>, from /boot/config-<release> or /proc/config.gz."""
    release = os.uname().release
    try:
        with open(f"/boot/config-{release}") as config:
            return f"CONFIG_{option}=y" in config.read().split()
    except OSError:
        return False


LOCKDEP = kernel_has("PROVE_LOCKING")
KASAN = kernel_has("KASAN")
KMEMLEAK = kernel_has("DEBUG_KMEMLEAK") and os.path.exists("/sys/kernel/debug/kmemleak")
ATOMIC_SLEEP = kernel_has("DEBUG_ATOMIC_SLEEP")
with open("/proc/version") as version:
    print("kernel:", version.read().strip())
print(f"machine: {os.uname().machine}; lockdep: {LOCKDEP}; KASAN: {KASAN}; kmemleak: {KMEMLEAK}; "
      f"atomic-sleep checks: {ATOMIC_SLEEP}")
# What any of the kernel's checkers prints when it finds something.
REPORTS = ("Oops", "BUG", "WARNING", "panic", "Call trace", "KASAN", "possible circular locking",
           "inconsistent lock state", "possible recursive locking", "sleeping function called", "kmemleak")


def kmemleak_scan():
    """The unreferenced blocks kmemleak finds now. It reports a block only on the scan after the one that first
    sees it, so this scans twice."""
    for _ in range(2):
        with open("/sys/kernel/debug/kmemleak", "w") as control:
            control.write("scan")
    with open("/sys/kernel/debug/kmemleak") as report:
        return report.read()


if KMEMLEAK:
    with open("/sys/kernel/debug/kmemleak", "w") as control:
        control.write("clear")

subprocess.run(["dmesg", "-C"], check=True)
subprocess.run(["insmod", "ceesafe_mq.ko"], check=True)
try:
    fd = os.open(DEVICE, os.O_RDWR)
    check("an empty queue reads 0 bytes", os.read(fd, MESSAGE_MAX) == b"")

    messages = [b"first", b"second message", bytes(range(256)) * 2]
    for message in messages:
        check(f"write of {len(message)} bytes", os.write(fd, message) == len(message))
    check("messages come back whole and in order", [os.read(fd, MESSAGE_MAX) for _ in messages] == messages)
    check("the queue is empty again", os.read(fd, MESSAGE_MAX) == b"")

    os.write(fd, b"0123456789")
    check("a short read takes the message's start", os.read(fd, 4) == b"0123")
    check("and the rest of it is gone", os.read(fd, MESSAGE_MAX) == b"")

    check("a message over the limit is EMSGSIZE", errno_of(lambda: os.write(fd, b"x" * (MESSAGE_MAX + 1))) == errno.EMSGSIZE)

    for i in range(SLOTS):
        os.write(fd, b"message %d" % i)
    check("a write to a full queue is ENOSPC", errno_of(lambda: os.write(fd, b"one too many")) == errno.ENOSPC)
    check("the oldest of a full queue is read first", os.read(fd, MESSAGE_MAX) == b"message 0")

    libc = ctypes.CDLL(None, use_errno=True)
    libc.write.argtypes = [ctypes.c_int, ctypes.c_void_p, ctypes.c_size_t]
    libc.write.restype = ctypes.c_ssize_t
    result = libc.write(fd, ctypes.c_void_p(8), 16)
    check("a write from a bad user address is EFAULT", result == -1 and ctypes.get_errno() == errno.EFAULT)
    libc.read.argtypes = [ctypes.c_int, ctypes.c_void_p, ctypes.c_size_t]
    libc.read.restype = ctypes.c_ssize_t
    result = libc.read(fd, ctypes.c_void_p(8), 16)
    check("a read to a bad user address is EFAULT", result == -1 and ctypes.get_errno() == errno.EFAULT)

    check("the failed read left its message queued", os.read(fd, MESSAGE_MAX) == b"message 1")

    # Written: 3 + 1 + 16 = 20. Read: 3 + 1 + 2 = 6. Refused writes: EMSGSIZE, ENOSPC, EFAULT = 3.
    # Still queued, for rmmod to free: 16 - 2 = 14.
    check("rmmod is refused while the device is open", subprocess.run(["rmmod", "ceesafe_mq"], capture_output=True).returncode != 0)
    os.close(fd)
finally:
    subprocess.run(["rmmod", "ceesafe_mq"], check=False)

log = kernel_log()
print(log.strip())
check("the module said it was ready", "ceesafe_mq: ready, 16 messages of at most 512 bytes" in log)
check("its counters are right", "ceesafe_mq: written 20, read 6, refused 3, freed with the device 14" in log, log)
check("unloading freed every kernel block", "ceesafe_mq: unloaded, 0 kernel block(s) live" in log)
check("no report from the kernel's checkers", not any(word in log for word in REPORTS))

# A second load: four processes write and read at once, on as many CPUs, so the mutex and the spinlock are
# contended. What was written is either read or freed with the device, and no block is left.
import re

subprocess.run(["dmesg", "-C"], check=True)
subprocess.run(["insmod", "ceesafe_mq.ko"], check=True)
try:
    workers = []
    for worker in range(4):
        pid = os.fork()
        if pid == 0:
            fd = os.open(DEVICE, os.O_RDWR)
            payload = bytes([worker]) * 300
            for _ in range(5000):
                try:
                    os.write(fd, payload)
                except OSError as error:
                    if error.errno != errno.ENOSPC:
                        os._exit(1)
                data = os.read(fd, MESSAGE_MAX)
                if data and (len(data) != 300 or data != bytes([data[0]]) * 300):
                    os._exit(2)  # a message was torn or mixed with another
            os._exit(0)
        workers.append(pid)
    statuses = [os.waitpid(pid, 0)[1] for pid in workers]
    check("four concurrent writers and readers saw only whole messages", statuses == [0, 0, 0, 0], str(statuses))
finally:
    subprocess.run(["rmmod", "ceesafe_mq"], check=False)
log = kernel_log()
print(log.strip())
counts = re.search(r"written (\d+), read (\d+), refused (\d+), freed with the device (\d+)", log)
check("the stress run's counters were logged", counts is not None)
if counts:
    written, read, refused, freed = (int(n) for n in counts.groups())
    check("every message written was read or freed", written == read + freed and written + refused == 20000, counts.group(0))
check("the stress run freed every kernel block", "ceesafe_mq: unloaded, 0 kernel block(s) live" in log)
check("no report from the kernel's checkers in the stress run", not any(word in log for word in REPORTS))
if KMEMLEAK:
    leaks = kmemleak_scan()
    check("kmemleak finds no unreferenced block after both runs", leaks.strip() == "", leaks[:2000])

# Controls: that the checkers which found nothing above do report a mistake when there is one. They run last,
# since KASAN reports only its first finding.
if ATOMIC_SLEEP and os.path.exists("control/sleep_under_spinlock.ko"):
    subprocess.run(["dmesg", "-C"], check=True)
    subprocess.run(["insmod", "control/sleep_under_spinlock.ko"], check=False)
    subprocess.run(["rmmod", "sleep_under_spinlock"], check=False)
    check("control: the C module that allocates under a spinlock is reported at run time",
          "BUG: sleeping function called from invalid context" in kernel_log())
if KASAN and subprocess.run(["modprobe", "lkdtm"], capture_output=True).returncode == 0:
    subprocess.run(["dmesg", "-C"], check=True)
    with open("/sys/kernel/debug/provoke-crash/DIRECT", "w") as provoke:
        provoke.write("READ_AFTER_FREE")
    check("control: KASAN reports lkdtm's read after free", "BUG: KASAN:" in kernel_log())
print(f"{len(failures)} failure(s)")
sys.exit(1 if failures else 0)
