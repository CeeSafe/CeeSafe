# ADR 0140 — `ProcessSignal.Hangup`: SIGHUP for a subscribed program

- Status: **Accepted** by the owner, 2026-11-15: asked whether to fix cee-proxy's F10 in CeeSafe, the answer was
  "Fix it and release". Reported by cee-proxy (`~/dev/CeeSafeApps/cee-proxy/CEESAFE_FINDINGS.md` F10), which needs
  `kill -HUP` and systemd's `ExecReload` to reload its configuration. Implemented on branch `fix/cee-proxy-f10`.
- Scope: lib/std (`lib/std/host.cee`: one more variant of `ProcessSignal`); the hosted runtime
  (`runtime/hosted/ceesafe_rt_async.c`, the POSIX handler); the interpreter (`src/interpreter/signals.cee`);
  type-system.md §26.15, c-backend.md §6, `guide/async.md`, `guide/builtins.md`, the book's §21.5;
  `tools/signal_check.py`; the corpus. No change to the checker and no new diagnostic. It amends ADR 0125 §2's "Which
  requests".

## 1. Context

- **ADR 0125** gave a program `SIGINT` and `SIGTERM`, and left every other signal alone. Its "More signals"
  alternative said `ProcessSignal` could grow "at the cost of users' exhaustive matches", once something asked.
- **What asked.** cee-proxy, a reverse proxy, reloads its configuration while it serves. The usual ways to ask a
  daemon for that are `kill -HUP <pid>` and systemd's `ExecReload=/bin/kill -HUP $MAINPID`. With ADR 0125, `SIGHUP`
  keeps its default action, which ends the process: the reload request stops the server, with the connections in
  flight cut. cee-proxy's workaround polls the file's modification time.
- **What `SIGHUP` means.** The terminal that controlled the process went away (a closed ssh session, a closed
  terminal window). A daemon has no terminal, so the signal is free for it, and the convention since the 1980s
  (inetd, syslogd, nginx, sshd, HAProxy's master) is "read your configuration again".
- **Who matches on `ProcessSignal`.** In CeeSafe's tree: the corpus test of ADR 0125 and `tools/signal_check.py`. In
  the owner's other projects (CeeSafe.Web, CeeSafeLibs, the apps): none; cee-proxy and the book's server only wait
  for any request.

## 2. Decision

```ceesafe
public enum ProcessSignal { Interrupt, Terminate, Hangup }
```

- **`Hangup`** is POSIX `SIGHUP`. While a subscription lives, `SIGHUP` is kept for the program like the other two:
  queued, returned by `poll`, `wait` and `waitAsync`, merged with the others when nobody takes them, and the default
  action restored when the subscription is dropped.
- **Always with the subscription.** A program that subscribes gets all three. One that never subscribes keeps every
  default, so `SIGHUP` still ends it.
- **What the program does with it is its own choice.** A server that stops at any request (`let _ = await
  signals.waitAsync()?;`) now stops in good order on `SIGHUP` too, where before it was killed. A daemon matches
  `Hangup` and reloads.
- **Windows** has no such request (a console's close is `Terminate`), so `Hangup` never comes there.
- **The runtime's value** sent into the subscription's channel is 3, and its bit in the handler's word is 4
  (`CEE_SIGNAL_HANGUP`, `CEE_SIGNAL_HANGUP_BIT`).

## 3. Alternatives considered

- **Opt in** (`ProcessSignals.subscribeWith(hangup: true)`): a subscribed program that does not ask would still be
  killed by `SIGHUP`. That keeps today's behaviour for it, but the enum still grows, so its exhaustive matches still
  need the arm. Two ways to subscribe for one more signal was not worth it.
- **A separate `ReloadRequests` type for `SIGHUP`:** a second handler, pipe and thread for one signal, and a program
  that wants both waits on two things. One queue in arrival order is what ADR 0125 chose.
- **`SIGUSR1`, `SIGUSR2`, `SIGQUIT`, `SIGWINCH`:** nothing has asked. Each would grow the enum again. `SIGHUP` is the
  one nearly every POSIX daemon needs.
- **Name it `Reload`:** the signal is not only that (a terminal's hangup), and the name would claim a meaning the
  runtime does not give it.

## 4. Compatibility

- **A source change** for an exhaustive `match` on `ProcessSignal`: it needs a `Hangup` arm (N0256, "this match does not
  cover ProcessSignal.Hangup", at the `match`), or a `_` arm. `is`, `if`, `poll() is null` and waiting for any request are unchanged.
- **A behaviour change** for a program that subscribes: `SIGHUP` is queued for it instead of ending it.
- A program that never subscribes behaves exactly as before.

## 5. Tests

- `tools/signal_check.py` sends `SIGHUP` in every case it sent `SIGINT` and `SIGTERM` in: a server of tasks (each
  reactor), a server of threads, one signal taken and then the default after the drop, no subscription at all (where
  `SIGHUP` must end the process with its default). Built and interpreted.
- Corpus `run-pass/process-signals.cee` matches all three variants.

## 6. Not in this ADR

- Other signals, signal masks, sending signals.
- What cee-proxy does with `Hangup` (it reloads, in its own repository).

## 7. Implementation and evidence

See ROADMAP §10's row for the tests run.
