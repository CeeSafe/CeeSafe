# Testing

A test is a function marked `@test`, and `ceesafec test` runs a package's tests, each as its own process.

## Writing tests

```ceesafe
/// The bytes 0..n, or the allocation failure.
fn numbers(n: u64) -> Result<Vector<u8>, AllocError> {
    var v = Vector<u8>.create();
    for (k in 0..n) {
        v.push(truncate<u8>(k))?;
    }
    return Ok(v);
}

@test
fn counts() -> Result<void, TestFailure> {
    let made = numbers(3);
    CheckResult<Vector<u8>, AllocError>.isOk(borrow made, "three bytes fit")?;
    match (made) {
        Ok(v) => {
            Check.equalU64(3, v.length as u64)?;
            let expected = [0u8, 1, 2];
            Check.equalBytes(expected[..], v.view())?;
        }
        Err(_) => { return Check.fail("unreachable after isOk"); }
    }
    return Ok();
}

@test
async fn waits() -> Result<void, AsyncError> {
    await Timer.sleep(1)?;
    return Ok();
}
```

- **Shape.** A test takes no parameters and returns `void` or `Result<void, E>`. An `Err` is its failure, and so is a
  panic. It may be `async`.
- **Checks.** `Check`'s functions (`isTrue`, `equalU64`, `equalText`, `equalBytes`, `fail`, …) return
  `Result<void, TestFailure>`, so a failed check returns from the test with `?`, dropping what it holds as any return
  does.
- **Where.** Tests go beside the code, or in the package's `tests/` directory (`tests = [...]` in the manifest). They
  see the package's items that are not public, except those marked `private`, which stay in their own file.
- **`build` leaves them out**, so calling a test from other code is an error (N0348).

## Running them

```text
ceesafec test                          every test
ceesafec test --filter parse --jobs 4  those whose name contains "parse", four at a time
ceesafec test --interp                 in the interpreter: a virtual clock, a fixed run order
ceesafec test --sweep alloc            each passing test again, once per allocation, that allocation failing
ceesafec test --sweep cancel           each passing async test again, cancelled at each of its suspensions
ceesafec test --format json            one JSON document, for CI
```

Each test runs as its own process, so a panic or a crash ends only that test. Tests run in the order of their names.
Each is reported as passed, failed, panicked, crashed (a signal), exited (it called `Process.exit` itself), timed out
(`--timeout`, 60 seconds by default) or leaked, natively and with `--interp`. A leak fails a test: CeeSafe has no
garbage collector, so memory still allocated at the end is a bug. A failing test's output is shown under its name, and
a test failing with its own enum names the variant. A `--filter` that matches no test is an error, and `--sweep` runs
natively only. `ceesafec test --help` lists the options.
