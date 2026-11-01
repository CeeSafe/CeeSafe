# CeeSafe fixed point (ROADMAP P6)

commit: 6ad8ba6bd065d634078b750a50ba8615b105bcba
host: Darwin arm64 27.0.0
stage 0: ceesafec 0.0.0-dev (/private/tmp/claude-501/-Users-kutty-reference/e35a115e-626e-4067-923d-96dc1b8520fb/scratchpad/s6/stage0-ceesafec, sha256 4c4de1b17a7aed13f3f97857922c63bbbf260b4943906ae5a971273a81dbca38)
cc: cc: Apple clang version 21.0.0 (clang-2100.3.34.2)
switches: (none: the default form)
runs: /private/tmp/claude-501/-Users-kutty-reference/e35a115e-626e-4067-923d-96dc1b8520fb/scratchpad/s6/fp/a and /private/tmp/claude-501/-Users-kutty-reference/e35a115e-626e-4067-923d-96dc1b8520fb/scratchpad/s6/fp/deeper/second-run/b
result: FIXED POINT

## Executables (sha256)
- gen1: run a 096b8151b4441859ced9153875bf794f772181b0f1c3c9544e82cbb9139a8723, run b 096b8151b4441859ced9153875bf794f772181b0f1c3c9544e82cbb9139a8723
- gen2: run a 096b8151b4441859ced9153875bf794f772181b0f1c3c9544e82cbb9139a8723, run b 096b8151b4441859ced9153875bf794f772181b0f1c3c9544e82cbb9139a8723
- gen3: run a 096b8151b4441859ced9153875bf794f772181b0f1c3c9544e82cbb9139a8723, run b 096b8151b4441859ced9153875bf794f772181b0f1c3c9544e82cbb9139a8723

## C emitted by gen 1, 2 and 3 (sha256; equal in both runs)
- main.c: 21897e0a3b237615b42a9489a62efba1ea4cf4d8836873eb7159e3ce6b26f800 (c2 = c3 = c4)
- ceesafe/rt.h: 8060867c71f39c1e037f06d7ce6707df82e79e039a89e8c7d7ab95384e38429d (c2 = c3 = c4)
- ceesafe_rt_core.c: 5bae77d011fa23a6376a78eec808e2599f4ad928fa748f29b1f7a362acf90576 (c2 = c3 = c4)
- ceesafe_rt_float.c: 5ae2f3959ae2cd2ade6778755d268b402c271193d6e56b62fe445469849ef111 (c2 = c3 = c4)
- ceesafe_rt_hosted.c: 4144279bbafef77b1736ed057ecdb7d1cb4c61fd70366432e6fde4fc7fac0cde (c2 = c3 = c4)
- ceesafe_rt_io.c: abeb42d14b260bbf00369fcbf9d61cfafc8bed82062f20fb795874a2906effb3 (c2 = c3 = c4)
- ceesafe_rt_thread.c: cad74d3b8ba440feb1b9efe01bce1a467bd392e6772411274338b6dbe7bb39fb (c2 = c3 = c4)

## Subprocess audit (gen 1 onward; PATH = the shims only)
- run a: 2 processes, programs: cc
- run b: 2 processes, programs: cc

## Commands

    $ /private/tmp/claude-501/-Users-kutty-reference/e35a115e-626e-4067-923d-96dc1b8520fb/scratchpad/s6/stage0-ceesafec build --cc cc -o /private/tmp/claude-501/-Users-kutty-reference/e35a115e-626e-4067-923d-96dc1b8520fb/scratchpad/s6/fp/a/gen/1/ceesafec   (in ceesafe)
    $ /private/tmp/claude-501/-Users-kutty-reference/e35a115e-626e-4067-923d-96dc1b8520fb/scratchpad/s6/fp/a/gen/1/ceesafec build --emit c -o /private/tmp/claude-501/-Users-kutty-reference/e35a115e-626e-4067-923d-96dc1b8520fb/scratchpad/s6/fp/a/gen/c2   (in ceesafe)
    $ /private/tmp/claude-501/-Users-kutty-reference/e35a115e-626e-4067-923d-96dc1b8520fb/scratchpad/s6/fp/a/gen/1/ceesafec build --cc cc -o /private/tmp/claude-501/-Users-kutty-reference/e35a115e-626e-4067-923d-96dc1b8520fb/scratchpad/s6/fp/a/gen/2/ceesafec   (in ceesafe)
    $ /private/tmp/claude-501/-Users-kutty-reference/e35a115e-626e-4067-923d-96dc1b8520fb/scratchpad/s6/fp/a/gen/2/ceesafec build --emit c -o /private/tmp/claude-501/-Users-kutty-reference/e35a115e-626e-4067-923d-96dc1b8520fb/scratchpad/s6/fp/a/gen/c3   (in ceesafe)
    $ /private/tmp/claude-501/-Users-kutty-reference/e35a115e-626e-4067-923d-96dc1b8520fb/scratchpad/s6/fp/a/gen/2/ceesafec build --cc cc -o /private/tmp/claude-501/-Users-kutty-reference/e35a115e-626e-4067-923d-96dc1b8520fb/scratchpad/s6/fp/a/gen/3/ceesafec   (in ceesafe)
    $ /private/tmp/claude-501/-Users-kutty-reference/e35a115e-626e-4067-923d-96dc1b8520fb/scratchpad/s6/fp/a/gen/3/ceesafec build --emit c -o /private/tmp/claude-501/-Users-kutty-reference/e35a115e-626e-4067-923d-96dc1b8520fb/scratchpad/s6/fp/a/gen/c4   (in ceesafe)
    $ /private/tmp/claude-501/-Users-kutty-reference/e35a115e-626e-4067-923d-96dc1b8520fb/scratchpad/s6/stage0-ceesafec build --cc cc -o /private/tmp/claude-501/-Users-kutty-reference/e35a115e-626e-4067-923d-96dc1b8520fb/scratchpad/s6/fp/deeper/second-run/b/gen/1/ceesafec   (in ceesafe)
    $ /private/tmp/claude-501/-Users-kutty-reference/e35a115e-626e-4067-923d-96dc1b8520fb/scratchpad/s6/fp/deeper/second-run/b/gen/1/ceesafec build --emit c -o /private/tmp/claude-501/-Users-kutty-reference/e35a115e-626e-4067-923d-96dc1b8520fb/scratchpad/s6/fp/deeper/second-run/b/gen/c2   (in ceesafe)
    $ /private/tmp/claude-501/-Users-kutty-reference/e35a115e-626e-4067-923d-96dc1b8520fb/scratchpad/s6/fp/deeper/second-run/b/gen/1/ceesafec build --cc cc -o /private/tmp/claude-501/-Users-kutty-reference/e35a115e-626e-4067-923d-96dc1b8520fb/scratchpad/s6/fp/deeper/second-run/b/gen/2/ceesafec   (in ceesafe)
    $ /private/tmp/claude-501/-Users-kutty-reference/e35a115e-626e-4067-923d-96dc1b8520fb/scratchpad/s6/fp/deeper/second-run/b/gen/2/ceesafec build --emit c -o /private/tmp/claude-501/-Users-kutty-reference/e35a115e-626e-4067-923d-96dc1b8520fb/scratchpad/s6/fp/deeper/second-run/b/gen/c3   (in ceesafe)
    $ /private/tmp/claude-501/-Users-kutty-reference/e35a115e-626e-4067-923d-96dc1b8520fb/scratchpad/s6/fp/deeper/second-run/b/gen/2/ceesafec build --cc cc -o /private/tmp/claude-501/-Users-kutty-reference/e35a115e-626e-4067-923d-96dc1b8520fb/scratchpad/s6/fp/deeper/second-run/b/gen/3/ceesafec   (in ceesafe)
    $ /private/tmp/claude-501/-Users-kutty-reference/e35a115e-626e-4067-923d-96dc1b8520fb/scratchpad/s6/fp/deeper/second-run/b/gen/3/ceesafec build --emit c -o /private/tmp/claude-501/-Users-kutty-reference/e35a115e-626e-4067-923d-96dc1b8520fb/scratchpad/s6/fp/deeper/second-run/b/gen/c4   (in ceesafe)
