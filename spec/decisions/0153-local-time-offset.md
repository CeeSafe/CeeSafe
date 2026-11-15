# ADR 0153 — The local time zone's offset

- Status: **Accepted** by the owner, 2026-11-15 ("Go ahead I accept both 1 and 2"), from ClayWind's finding N1: its
  chat client, P3's gate app, shows each message's time and can only show UTC.
- Scope: `lib/std/host.cee` (`Clock.localOffset`), `runtime/hosted/ceesafe_rt_io.c`, the interpreter
  (`src/interpreter/io.cee`), type-system.md §26.11, `spec/api/std.md` (generated), `tools/clock_check.py`. It amends
  ADR 0055, which left time zones out of the library.

## 1. Context

- **ADR 0055 gave the date, not the zone.** `Clock.now()` is a `UnixTime`, seconds since 1970 in UTC, and §26.11 says
  "calendar dates, time zones and formatting are not part of the library".
- **A program that shows a time to a person needs the zone.** ClayWind's chat client prints each message's hour and
  minute from the server's Unix time: in UTC, three hours off for its owner (UTC+3). An app cannot work the offset out:
  the zone is the system's setting (`TZ`, `/etc/localtime`, Windows' time zone), and the rules for daylight saving are
  the system's database. Reading either without C is not possible in safe code, and calling C for it is the
  workaround the project rules forbid.
- **Only the offset is missing.** With seconds east of UTC, an app turns a `UnixTime` into a local hour and minute,
  or a local date, with integer arithmetic it already does for UTC.

## 2. Decision

```ceesafe
let now = Clock.now();
let local = now.seconds + Clock.localOffset(now) as i64;   // seconds since 1970 as the local wall clock reads
let minutes = (local % 86400 + 86400) % 86400 / 60;          // the local time of day, in minutes
```

- **`Clock.localOffset(at: UnixTime) -> i32`**: the seconds to add to UTC to get the local time at `at`, as the system's
  time zone gives it, daylight saving included: 10800 for UTC+3, -18000 for New York in winter and -14400 in summer,
  19800 for India. It is the offset at `at`, not now, so a stored time (a message's) is shown with the offset it had.
- **The system's zone and database.** POSIX systems: `localtime_r`'s `tm_gmtoff` (the `TZ` variable, else
  `/etc/localtime`). Windows: the current zone with its dynamic daylight-saving rules
  (`GetDynamicTimeZoneInformation`, `SystemTimeToTzSpecificLocalTimeEx`), the offset being the local time less UTC.
- **0 when there is none**: in the interpreter, so that runs stay reproducible, as its clock is (§40.8); where the
  system has no time zone (CosmoOS userland); and when the system cannot convert `at` (before 1601 on Windows, or a
  `time_t` it does not take).
- **No effects** (`@effects()`), as `Clock.now` and `Random.fill`: the first call may read the zone's file, once, as
  `Random.fill` reads the system's source; an async task may format a time.
- **Not added:** calendar dates, a zone's name, formatting, other zones than the system's. They are larger designs
  (a date type, a database of zones), and none is needed to show a local time.

## 3. Alternatives considered

- **Read `TZ` and the zone files in lib/std.** It would duplicate the C library's parser and database lookup, and
  Windows has neither.
- **`Clock.localOffsetNow()`.** An offset for now is wrong for a time from yesterday across a change of daylight
  saving; `localOffset(Clock.now())` says the same.
- **A `LocalTime` type with fields.** It is formatting's first step, and its fields are the arithmetic above; left for
  a date ADR if one is needed.

## 4. Consequences

- One new function, `Clock.localOffset`; one runtime call, `ceesafe_clock_local_offset(seconds) -> i32`.
- §26.11 no longer says time zones are not part of the library: the offset is, dates and formatting still are not.

## 5. Implementation and evidence (2026-11-15)

**Files:** `lib/std/host.cee`, `runtime/hosted/ceesafe_rt_io.c` and `rt.h`, `src/interpreter/io.cee` (0),
`tools/clock_check.py` (offsets under several `TZ` values against Python's `zoneinfo`, and the interpreter's 0),
type-system.md §26.11, `spec/api/std.md`.

**Tests** are recorded in ROADMAP §10 (2026-11-15, ADR 0153).
