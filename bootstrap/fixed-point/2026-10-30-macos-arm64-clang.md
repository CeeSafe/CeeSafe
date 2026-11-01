# CeeSafe fixed point (ROADMAP P6)

commit: 1b1aae42c8a1c86d756de0b7ce10f537957a1ede
host: Darwin arm64 27.0.0
stage 0: the reference compiler 0.6.0 (/Users/kutty/.cee/0.6.0/bin/the reference compiler, sha256 1d8b8f1f7eaea16f831bdf98014d5f51a65673cd50497e754cae218659d78374)
cc: cc: Apple clang version 21.0.0 (clang-2100.3.34.2)
runs: <scratchpad>/fp/a and <scratchpad>/fp/deeper/second-run/b
result: FIXED POINT

## Executables (sha256)
- gen1: run a 021f03492028781acdf6859dedd93f38a668eae406c8055fbc205ba52fb89ac7, run b 021f03492028781acdf6859dedd93f38a668eae406c8055fbc205ba52fb89ac7
- gen2: run a 00bf458c8b4b3a18121128632f05a3650a5c4a95387bcc4cf5887b7becc7bb04, run b 00bf458c8b4b3a18121128632f05a3650a5c4a95387bcc4cf5887b7becc7bb04
- gen3: run a 00bf458c8b4b3a18121128632f05a3650a5c4a95387bcc4cf5887b7becc7bb04, run b 00bf458c8b4b3a18121128632f05a3650a5c4a95387bcc4cf5887b7becc7bb04

## C emitted by gen 1, 2 and 3 (sha256; equal in both runs)
- main.c: 40374dbfa538ab615475f3a77ea49fc7129b92ef39f943ef108f26799fa1465a (c2 = c3 = c4)
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

    $ /Users/kutty/.cee/0.6.0/bin/ceesafec build --cc cc -o <scratchpad>/fp/a/gen/1/ceesafec   (in ceesafe)
    $ <scratchpad>/fp/a/gen/1/ceesafec build --emit c -o <scratchpad>/fp/a/gen/c2   (in ceesafe)
    $ <scratchpad>/fp/a/gen/1/ceesafec build --cc cc -o <scratchpad>/fp/a/gen/2/ceesafec   (in ceesafe)
    $ <scratchpad>/fp/a/gen/2/ceesafec build --emit c -o <scratchpad>/fp/a/gen/c3   (in ceesafe)
    $ <scratchpad>/fp/a/gen/2/ceesafec build --cc cc -o <scratchpad>/fp/a/gen/3/ceesafec   (in ceesafe)
    $ <scratchpad>/fp/a/gen/3/ceesafec build --emit c -o <scratchpad>/fp/a/gen/c4   (in ceesafe)
    $ /Users/kutty/.cee/0.6.0/bin/ceesafec build --cc cc -o <scratchpad>/fp/deeper/second-run/b/gen/1/ceesafec   (in ceesafe)
    $ <scratchpad>/fp/deeper/second-run/b/gen/1/ceesafec build --emit c -o <scratchpad>/fp/deeper/second-run/b/gen/c2   (in ceesafe)
    $ <scratchpad>/fp/deeper/second-run/b/gen/1/ceesafec build --cc cc -o <scratchpad>/fp/deeper/second-run/b/gen/2/ceesafec   (in ceesafe)
    $ <scratchpad>/fp/deeper/second-run/b/gen/2/ceesafec build --emit c -o <scratchpad>/fp/deeper/second-run/b/gen/c3   (in ceesafe)
    $ <scratchpad>/fp/deeper/second-run/b/gen/2/ceesafec build --cc cc -o <scratchpad>/fp/deeper/second-run/b/gen/3/ceesafec   (in ceesafe)
    $ <scratchpad>/fp/deeper/second-run/b/gen/3/ceesafec build --emit c -o <scratchpad>/fp/deeper/second-run/b/gen/c4   (in ceesafe)
