# ADR 0149 — FreeBSD: a released ceesafec, and CeeSafe's tests there

- Status: **Accepted** by the owner, 2026-11-15. Asked for by CeeSafe.Web as U23
  (`~/dev/CeeSafe.Web/docs/ceesafe-upstream-work.md`, found 2026-11-11 under its Web ADR 0015 §5: "wanted before v1.0,
  otherwise FreeBSD is best effort"). Asked whether CI and the release should build FreeBSD in a VM inside an Ubuntu
  job with `vmactions/freebsd-vm` pinned to a commit, the owner chose "Yes, vmactions (Recommended)", and "One release,
  0.52.0 (Recommended)": FreeBSD brought up in CI first, then released with ADRs 0147 and 0148. Meanwhile 0.51.2 and
  0.52.0 were released from main for other work, so the three are released as 0.53.0. Implemented on branch
  `release-0.53.0-prep` (first named `ceesafe-0.52.0`, the version then expected).
- Scope: the compiler's host checks (`src/host/toolchain.cee`, `src/host/embedding.cee`,
  `src/packages/manifest.cee`), the hosted runtime (`ceesafe_rt_async.c`, `ceesafe_rt_window.c`),
  `.github/workflows/freebsd.yml` (new) and `release.yml`, `site/public/install.sh`, tooling.md §4.1 and §8.3,
  ADR 0047's note, guide/installation.md and guide/packages.md, docs/release.md.

## 1. Context

- **Releases** (0.51.1) shipped ceesafec for linux-x64, linux-arm64, osx-x64, osx-arm64, win-x64 and win-arm64, and a
  C seed whose README names "the BSDs" among the systems `cc` builds it on. Nothing checked that, and no CeeSafe CI
  ran on FreeBSD.
- **The runtime** already took the kqueue path for `__FreeBSD__` (`ceesafe_rt_async.c`), untested.
- **The compiler** knew three systems. With no `/proc/self`, FreeBSD was taken for macOS (`manifestTarget`), and
  `embed` refused every file there ("ceesafec embeds files only on Linux, macOS and Windows").
- **GitHub has no FreeBSD runner.** None of the owner's machines runs FreeBSD.

## 2. Decision

### 2.1 FreeBSD as a host

- **Known by `uname -s`** (`FreeBSD`): `hostIsFreeBsd` (toolchain.cee), and `hostSystem`'s 4 (embedding.cee).
- **`[target.freebsd]`** in the manifest (ADR 0047), applied on FreeBSD. FreeBSD is asked before Linux's
  `/proc/self`, which FreeBSD has too when procfs is mounted.
- **`-lpthread`** after a hosted program's objects (tooling.md §8.3): FreeBSD's threads are libthr, which libc does not
  hold, as glibc 2.34's and macOS's do.
- **`embed` and `embedText`** check the open descriptor through `fcntl(F_KINFO)` (FreeBSD 13.1 on), whose
  `struct kinfo_file` (sys/user.h) gives the path the descriptor was opened by (`kf_path`) and the vnode's type, as
  `F_GETPATH` does on macOS and `/proc/self/fd` on Linux. FreeBSD's open flags are its own (`O_NONBLOCK` 0x4,
  `O_NOCTTY` 0x8000, `O_CLOEXEC` 0x100000). The layout is amd64's, the one FreeBSD target released.
- **The runtime:**
  - a datagram's IPv4 source (ADR 0050's `from`) goes through `IP_SENDSRCADDR` with an `in_addr`: FreeBSD has no
    `IP_PKTINFO` (ip(4)); receiving already used `IP_RECVDSTADDR` where `IP_PKTINFO` is missing;
  - windows (ADR 0069): the X11 backend, libX11.so.6 loaded with dlopen as on Linux, and `pthread_main_np` for "the
    program's first thread".

### 2.2 CI and the release

- **`.github/workflows/freebsd.yml`**, on release tags and by hand, like checks.yml (it does not hold a release back):
  an Ubuntu job builds the tree's ceesafec with a released one and writes its C; a FreeBSD 14.4 x86_64 VM
  (`vmactions/freebsd-vm`, pinned to v1.6.0's commit) compiles that C with `cc` alone, as the seed's README says, lets
  the result rebuild ceesafec with its own `build`, checks that both emit the same C, runs the release's smoke test,
  and then the corpus (every family, and run-pass and run-fail again with leak checks), the support library's tests,
  the package tests and the platform checks (`ceesafec test`, frame stacks, UDP, the clock, random bytes, wakes).
- **`release.yml`'s `freebsd` job**, which `publish` needs: the build job's steps for FreeBSD, in the same VM, from the
  release's seed: generation 1 by `cc`, generation 2 by generation 1, both emitting the seed byte for byte; the smoke
  test; `ceesafec-freebsd-x64.tar.gz` into SHA256SUMS. No secret reaches it.
- **`install.sh`** knows FreeBSD (x86_64 only), and falls back to FreeBSD's base `fetch` and `sha256` where `curl`,
  `wget`, `sha256sum` and `shasum` are missing.
- **Built on 14.4**, so it runs on FreeBSD 14.4 and later (guide/installation.md).

### 2.3 Not taken

- **A FreeBSD arm64 build.** The VM has one (`arch: aarch64`), under emulation, so slow; there is no request for it.
- **sendfile on FreeBSD.** FreeBSD's sendfile(2) is not Linux's; `File.sendTo` keeps the copy it uses on macOS
  (`CEE_ASYNC_NO_SENDFILE`).
- **The interpreter's errno table** stays Linux's or the BSDs': FreeBSD's numbers for the errors the interpreter
  reports are macOS's.

## 3. Consequences

- `install.sh` installs ceesafec on FreeBSD x86_64, and `ceesafec build` works there with the base system's clang.
- A package can name FreeBSD's libraries (`[target.freebsd]`).
- Every release builds and smoke-tests FreeBSD, and a FreeBSD failure holds the release back, as any platform's does.
- The VM action is a third-party action in the release workflow, pinned to a commit; its job has no secret and only
  produces an artifact that `publish` checksums with the rest.

- **Also found and fixed on the way:**
  - The record cache of interface records (ADR 0082) reads fstat's layout, known only for Linux and macOS: on FreeBSD
    `statLayout` gives 0, so no record is made and every check is the full one, as with a cold cache. Correct, and
    slower for a warm check. `tools/package_tests.py` skips its two record checks there.
  - `run-pass/async-tcp-vectored` set a 4,096-byte receive buffer as well as the send buffer. On FreeBSD's `lo0`
    (an MSS of about 16 KiB) such a window never opens by the amount that makes the receiver send a window update,
    so the sender waited for TCP's persist timer, and the program took 77 s (truss: every send's `EAGAIN` followed by
    a wait for writability that only the timer ended; the same on the poll() fallback). Its generator now makes the
    sender's buffer small only: 3.1 s, still with 144 `EAGAIN`s, so the sends still stop inside parts. The expected
    lines are unchanged.

## 4. Tests

Run on 2026-11-15.

- **`freebsd.yml` on this branch** (GitHub Actions, FreeBSD 14.4 amd64 in a VM): the first run stopped at the seed's
  compile (`IP_PKTINFO`, §2.1). From the second on: generation 1 built by `cc` from the C alone, generation 2 by
  generation 1, both emitting the same C ("the fixed point holds on FreeBSD"); the smoke test passes (the interpreter,
  a native build, a scripted window, the MCP server). The third: the corpus, every family, 0 failures but
  `run-pass/async-tcp-vectored` (above), and the same under leak checks; `tests/support` 9 of 9;
  `test_command_check.py` 7 checks, `frame_stack_check.py`, `udp_check.py` (both reactors), `clock_check.py`,
  `random_check.py`, `wake_check.py`: 0 problems each.
- **On the owner's FreeBSD 15.1 amd64 PC** (clang 19.1.7, 4 CPUs), a ceesafec built there by `cc` from this tree's C
  (126 s) and then by itself: the corpus, every family (the 4 freestanding files partial, as everywhere), and run-pass
  (396) and run-fail (55) again with leak checks: 0 failures; `package_tests.py` 36 checks (the records skipped),
  `cli_check.py`, `signal_check.py` (27 cases), `dns_check.py` (4 programs), `embed_check.py` (83 passed),
  `fault_sweep.py` on the support library (573 of 573 failure points handled), `fix_paths_check.py`: 0 problems each.
- **Not run on FreeBSD:** the sanitizer runs (`--no-sanitizers` where a check has the option), the contract suite as one
  command, the window runtime with a display, arm64.
