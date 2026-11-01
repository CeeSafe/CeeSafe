# CeeSafe fixed point (ROADMAP P6)

commit: f9f6d6f94446d74d085a4f83710f6faa835bbe20
host: Darwin arm64 27.0.0
stage 0: ceesafec 0.0.0-dev (/private/tmp/claude-501/-Users-kutty-reference/e35a115e-626e-4067-923d-96dc1b8520fb/scratchpad/s7/stage0-ceesafec, sha256 b21bfe555492b687ca6a0bf7bd95f968e23cd4f9089a712a8a1194ea250c2dba)
cc: cc: Apple clang version 21.0.0 (clang-2100.3.34.2)
switches: (none: the default form)
runs: /private/tmp/claude-501/-Users-kutty-reference/e35a115e-626e-4067-923d-96dc1b8520fb/scratchpad/s7/fp/a and /private/tmp/claude-501/-Users-kutty-reference/e35a115e-626e-4067-923d-96dc1b8520fb/scratchpad/s7/fp/deeper/second-run/b
result: FIXED POINT

## Executables (sha256)
- gen1: run a fce8e6b92bf1d9a706b396f0d2848bad92b4b9224cc79cc6d43bd2db50999f99, run b fce8e6b92bf1d9a706b396f0d2848bad92b4b9224cc79cc6d43bd2db50999f99
- gen2: run a fce8e6b92bf1d9a706b396f0d2848bad92b4b9224cc79cc6d43bd2db50999f99, run b fce8e6b92bf1d9a706b396f0d2848bad92b4b9224cc79cc6d43bd2db50999f99
- gen3: run a fce8e6b92bf1d9a706b396f0d2848bad92b4b9224cc79cc6d43bd2db50999f99, run b fce8e6b92bf1d9a706b396f0d2848bad92b4b9224cc79cc6d43bd2db50999f99

## C emitted by gen 1, 2 and 3 (sha256; equal in both runs)
- ceesafe/rt.h: 4dc3be97530325d3eb6bf0995cd6e9af3c9546d962e0639102efcc6e779e901a (c2 = c3 = c4)
- ceesafe_rt_core.c: ce8e9e6d4f0d10773b931c2b1ec29ce8dc08c22b75bdeff4d9afe3ec06fb8c91 (c2 = c3 = c4)
- ceesafe_rt_float.c: 5d21b1203bffd2052f483002e30da15764e99feade356da40e7d61a51292a875 (c2 = c3 = c4)
- ceesafe_rt_hosted.c: d3d600162a70b81f3181655c87c8047a4a486afc0bd969e48e2a5f7630333200 (c2 = c3 = c4)
- ceesafe_rt_io.c: 851d8f4782ed6ba9c0c68af0b9b7cee587ad25445c777c217981755d3d0fc857 (c2 = c3 = c4)
- ceesafe_rt_thread.c: 73298da9a97d061c95539d54f297ccd502e071cb24669b7dbcc5e61fdfb98a10 (c2 = c3 = c4)
- main.c: 65d199b3d1cb4127d4bc844b8c8d09c7b0920dd6366b3fe220a29d1afc9807ba (c2 = c3 = c4)

## Subprocess audit (gen 1 onward; PATH = the shims only)
- run a: 2 processes, programs: cc
- run b: 2 processes, programs: cc

## Commands

    $ /private/tmp/claude-501/-Users-kutty-reference/e35a115e-626e-4067-923d-96dc1b8520fb/scratchpad/s7/stage0-ceesafec build --cc cc -o /private/tmp/claude-501/-Users-kutty-reference/e35a115e-626e-4067-923d-96dc1b8520fb/scratchpad/s7/fp/a/gen/1/ceesafec   (in ceesafe)
    $ /private/tmp/claude-501/-Users-kutty-reference/e35a115e-626e-4067-923d-96dc1b8520fb/scratchpad/s7/fp/a/gen/1/ceesafec build --emit c -o /private/tmp/claude-501/-Users-kutty-reference/e35a115e-626e-4067-923d-96dc1b8520fb/scratchpad/s7/fp/a/gen/c2   (in ceesafe)
    $ /private/tmp/claude-501/-Users-kutty-reference/e35a115e-626e-4067-923d-96dc1b8520fb/scratchpad/s7/fp/a/gen/1/ceesafec build --cc cc -o /private/tmp/claude-501/-Users-kutty-reference/e35a115e-626e-4067-923d-96dc1b8520fb/scratchpad/s7/fp/a/gen/2/ceesafec   (in ceesafe)
    $ /private/tmp/claude-501/-Users-kutty-reference/e35a115e-626e-4067-923d-96dc1b8520fb/scratchpad/s7/fp/a/gen/2/ceesafec build --emit c -o /private/tmp/claude-501/-Users-kutty-reference/e35a115e-626e-4067-923d-96dc1b8520fb/scratchpad/s7/fp/a/gen/c3   (in ceesafe)
    $ /private/tmp/claude-501/-Users-kutty-reference/e35a115e-626e-4067-923d-96dc1b8520fb/scratchpad/s7/fp/a/gen/2/ceesafec build --cc cc -o /private/tmp/claude-501/-Users-kutty-reference/e35a115e-626e-4067-923d-96dc1b8520fb/scratchpad/s7/fp/a/gen/3/ceesafec   (in ceesafe)
    $ /private/tmp/claude-501/-Users-kutty-reference/e35a115e-626e-4067-923d-96dc1b8520fb/scratchpad/s7/fp/a/gen/3/ceesafec build --emit c -o /private/tmp/claude-501/-Users-kutty-reference/e35a115e-626e-4067-923d-96dc1b8520fb/scratchpad/s7/fp/a/gen/c4   (in ceesafe)
    $ /private/tmp/claude-501/-Users-kutty-reference/e35a115e-626e-4067-923d-96dc1b8520fb/scratchpad/s7/stage0-ceesafec build --cc cc -o /private/tmp/claude-501/-Users-kutty-reference/e35a115e-626e-4067-923d-96dc1b8520fb/scratchpad/s7/fp/deeper/second-run/b/gen/1/ceesafec   (in ceesafe)
    $ /private/tmp/claude-501/-Users-kutty-reference/e35a115e-626e-4067-923d-96dc1b8520fb/scratchpad/s7/fp/deeper/second-run/b/gen/1/ceesafec build --emit c -o /private/tmp/claude-501/-Users-kutty-reference/e35a115e-626e-4067-923d-96dc1b8520fb/scratchpad/s7/fp/deeper/second-run/b/gen/c2   (in ceesafe)
    $ /private/tmp/claude-501/-Users-kutty-reference/e35a115e-626e-4067-923d-96dc1b8520fb/scratchpad/s7/fp/deeper/second-run/b/gen/1/ceesafec build --cc cc -o /private/tmp/claude-501/-Users-kutty-reference/e35a115e-626e-4067-923d-96dc1b8520fb/scratchpad/s7/fp/deeper/second-run/b/gen/2/ceesafec   (in ceesafe)
    $ /private/tmp/claude-501/-Users-kutty-reference/e35a115e-626e-4067-923d-96dc1b8520fb/scratchpad/s7/fp/deeper/second-run/b/gen/2/ceesafec build --emit c -o /private/tmp/claude-501/-Users-kutty-reference/e35a115e-626e-4067-923d-96dc1b8520fb/scratchpad/s7/fp/deeper/second-run/b/gen/c3   (in ceesafe)
    $ /private/tmp/claude-501/-Users-kutty-reference/e35a115e-626e-4067-923d-96dc1b8520fb/scratchpad/s7/fp/deeper/second-run/b/gen/2/ceesafec build --cc cc -o /private/tmp/claude-501/-Users-kutty-reference/e35a115e-626e-4067-923d-96dc1b8520fb/scratchpad/s7/fp/deeper/second-run/b/gen/3/ceesafec   (in ceesafe)
    $ /private/tmp/claude-501/-Users-kutty-reference/e35a115e-626e-4067-923d-96dc1b8520fb/scratchpad/s7/fp/deeper/second-run/b/gen/3/ceesafec build --emit c -o /private/tmp/claude-501/-Users-kutty-reference/e35a115e-626e-4067-923d-96dc1b8520fb/scratchpad/s7/fp/deeper/second-run/b/gen/c4   (in ceesafe)
