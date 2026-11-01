# CeeSafe fixed point (ROADMAP P6)

commit: e401024efb4c5092858688969034af5fe7fe3231
host: Darwin arm64 27.0.0
stage 0: ceesafec 0.0.0-dev (/private/tmp/claude-501/-Users-kutty-reference/e35a115e-626e-4067-923d-96dc1b8520fb/scratchpad/fp-spec/ceesafec, sha256 f647da6b6819f484f2bda47de2d69d732aef4fda09dd6202aeb3fc387fa565bc)
cc: cc: Apple clang version 21.0.0 (clang-2100.3.34.2)
switches: (none: the default form)
runs: /private/tmp/claude-501/-Users-kutty-reference/e35a115e-626e-4067-923d-96dc1b8520fb/scratchpad/fp-spec/flat/a and /private/tmp/claude-501/-Users-kutty-reference/e35a115e-626e-4067-923d-96dc1b8520fb/scratchpad/fp-spec/flat/deeper/second-run/b
result: FIXED POINT

## Executables (sha256)
- gen1: run a 928908cc3f5a1e3ca0f0372cd01a5331aeda14f2c910d6028782da865760e5c5, run b 928908cc3f5a1e3ca0f0372cd01a5331aeda14f2c910d6028782da865760e5c5
- gen2: run a 928908cc3f5a1e3ca0f0372cd01a5331aeda14f2c910d6028782da865760e5c5, run b 928908cc3f5a1e3ca0f0372cd01a5331aeda14f2c910d6028782da865760e5c5
- gen3: run a 928908cc3f5a1e3ca0f0372cd01a5331aeda14f2c910d6028782da865760e5c5, run b 928908cc3f5a1e3ca0f0372cd01a5331aeda14f2c910d6028782da865760e5c5

## C emitted by gen 1, 2 and 3 (sha256; equal in both runs)
- ceesafe/rt.h: 7d1a225b96c7a0612e399a1bc502752c243aeb880eaf4111c6b9a3458b5d528d (c2 = c3 = c4)
- ceesafe_rt_core.c: 2daf2968471c3e1c0da9e038b9957642e54763aa047c3f0857ed86a5b8f43320 (c2 = c3 = c4)
- ceesafe_rt_float.c: 5d21b1203bffd2052f483002e30da15764e99feade356da40e7d61a51292a875 (c2 = c3 = c4)
- ceesafe_rt_hosted.c: cfbb8e82f3b2fe0f1eddf256037c936a5a7a866ab220aa0e3df28498526e344b (c2 = c3 = c4)
- ceesafe_rt_io.c: 8a411a39e6c43ab63cffc030a2d10a57661da8f6be2be3f18c525b3a66b4467e (c2 = c3 = c4)
- ceesafe_rt_thread.c: 73298da9a97d061c95539d54f297ccd502e071cb24669b7dbcc5e61fdfb98a10 (c2 = c3 = c4)
- main.c: 7de3bccdf274b54d78bd8cfb26093dd53f6e0d1ade9b9a1aaf976345b3abedf2 (c2 = c3 = c4)

## Subprocess audit (gen 1 onward; PATH = the shims only)
- run a: 2 processes, programs: cc
- run b: 2 processes, programs: cc

## Commands

    $ /private/tmp/claude-501/-Users-kutty-reference/e35a115e-626e-4067-923d-96dc1b8520fb/scratchpad/fp-spec/ceesafec build --cc cc -o /private/tmp/claude-501/-Users-kutty-reference/e35a115e-626e-4067-923d-96dc1b8520fb/scratchpad/fp-spec/flat/a/gen/1/ceesafec   (in ceesafe)
    $ /private/tmp/claude-501/-Users-kutty-reference/e35a115e-626e-4067-923d-96dc1b8520fb/scratchpad/fp-spec/flat/a/gen/1/ceesafec build --emit c -o /private/tmp/claude-501/-Users-kutty-reference/e35a115e-626e-4067-923d-96dc1b8520fb/scratchpad/fp-spec/flat/a/gen/c2   (in ceesafe)
    $ /private/tmp/claude-501/-Users-kutty-reference/e35a115e-626e-4067-923d-96dc1b8520fb/scratchpad/fp-spec/flat/a/gen/1/ceesafec build --cc cc -o /private/tmp/claude-501/-Users-kutty-reference/e35a115e-626e-4067-923d-96dc1b8520fb/scratchpad/fp-spec/flat/a/gen/2/ceesafec   (in ceesafe)
    $ /private/tmp/claude-501/-Users-kutty-reference/e35a115e-626e-4067-923d-96dc1b8520fb/scratchpad/fp-spec/flat/a/gen/2/ceesafec build --emit c -o /private/tmp/claude-501/-Users-kutty-reference/e35a115e-626e-4067-923d-96dc1b8520fb/scratchpad/fp-spec/flat/a/gen/c3   (in ceesafe)
    $ /private/tmp/claude-501/-Users-kutty-reference/e35a115e-626e-4067-923d-96dc1b8520fb/scratchpad/fp-spec/flat/a/gen/2/ceesafec build --cc cc -o /private/tmp/claude-501/-Users-kutty-reference/e35a115e-626e-4067-923d-96dc1b8520fb/scratchpad/fp-spec/flat/a/gen/3/ceesafec   (in ceesafe)
    $ /private/tmp/claude-501/-Users-kutty-reference/e35a115e-626e-4067-923d-96dc1b8520fb/scratchpad/fp-spec/flat/a/gen/3/ceesafec build --emit c -o /private/tmp/claude-501/-Users-kutty-reference/e35a115e-626e-4067-923d-96dc1b8520fb/scratchpad/fp-spec/flat/a/gen/c4   (in ceesafe)
    $ /private/tmp/claude-501/-Users-kutty-reference/e35a115e-626e-4067-923d-96dc1b8520fb/scratchpad/fp-spec/ceesafec build --cc cc -o /private/tmp/claude-501/-Users-kutty-reference/e35a115e-626e-4067-923d-96dc1b8520fb/scratchpad/fp-spec/flat/deeper/second-run/b/gen/1/ceesafec   (in ceesafe)
    $ /private/tmp/claude-501/-Users-kutty-reference/e35a115e-626e-4067-923d-96dc1b8520fb/scratchpad/fp-spec/flat/deeper/second-run/b/gen/1/ceesafec build --emit c -o /private/tmp/claude-501/-Users-kutty-reference/e35a115e-626e-4067-923d-96dc1b8520fb/scratchpad/fp-spec/flat/deeper/second-run/b/gen/c2   (in ceesafe)
    $ /private/tmp/claude-501/-Users-kutty-reference/e35a115e-626e-4067-923d-96dc1b8520fb/scratchpad/fp-spec/flat/deeper/second-run/b/gen/1/ceesafec build --cc cc -o /private/tmp/claude-501/-Users-kutty-reference/e35a115e-626e-4067-923d-96dc1b8520fb/scratchpad/fp-spec/flat/deeper/second-run/b/gen/2/ceesafec   (in ceesafe)
    $ /private/tmp/claude-501/-Users-kutty-reference/e35a115e-626e-4067-923d-96dc1b8520fb/scratchpad/fp-spec/flat/deeper/second-run/b/gen/2/ceesafec build --emit c -o /private/tmp/claude-501/-Users-kutty-reference/e35a115e-626e-4067-923d-96dc1b8520fb/scratchpad/fp-spec/flat/deeper/second-run/b/gen/c3   (in ceesafe)
    $ /private/tmp/claude-501/-Users-kutty-reference/e35a115e-626e-4067-923d-96dc1b8520fb/scratchpad/fp-spec/flat/deeper/second-run/b/gen/2/ceesafec build --cc cc -o /private/tmp/claude-501/-Users-kutty-reference/e35a115e-626e-4067-923d-96dc1b8520fb/scratchpad/fp-spec/flat/deeper/second-run/b/gen/3/ceesafec   (in ceesafe)
    $ /private/tmp/claude-501/-Users-kutty-reference/e35a115e-626e-4067-923d-96dc1b8520fb/scratchpad/fp-spec/flat/deeper/second-run/b/gen/3/ceesafec build --emit c -o /private/tmp/claude-501/-Users-kutty-reference/e35a115e-626e-4067-923d-96dc1b8520fb/scratchpad/fp-spec/flat/deeper/second-run/b/gen/c4   (in ceesafe)
