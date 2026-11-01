# CeeSafe fixed point (ROADMAP P6)

commit: 8941c10802e81bed787173c53c72a6cff7d1fe15
host: Darwin arm64 27.0.0
stage 0: ceesafec 0.0.0-dev (<scratchpad>/stage0-ceesafec, sha256 d236748ee449857f17ca96e299fc6144f2c2f1d26d7081e8da80421121f28b68)
cc: cc: Apple clang version 21.0.0 (clang-2100.3.34.2)
runs: <scratchpad>/fpr/a and <scratchpad>/fpr/deeper/second-run/b
result: FIXED POINT

## Executables (sha256)
- gen1: run a d236748ee449857f17ca96e299fc6144f2c2f1d26d7081e8da80421121f28b68, run b d236748ee449857f17ca96e299fc6144f2c2f1d26d7081e8da80421121f28b68
- gen2: run a d236748ee449857f17ca96e299fc6144f2c2f1d26d7081e8da80421121f28b68, run b d236748ee449857f17ca96e299fc6144f2c2f1d26d7081e8da80421121f28b68
- gen3: run a d236748ee449857f17ca96e299fc6144f2c2f1d26d7081e8da80421121f28b68, run b d236748ee449857f17ca96e299fc6144f2c2f1d26d7081e8da80421121f28b68

## C emitted by gen 1, 2 and 3 (sha256; equal in both runs)
- main.c: 4b73077ca55f5143465fdc93b15fdc24b5ecc518d00ea14324b1f1008f1f4b6b (c2 = c3 = c4)
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

    $ <scratchpad>/stage0-ceesafec build --cc cc -o <scratchpad>/fpr/a/gen/1/ceesafec   (in ceesafe)
    $ <scratchpad>/fpr/a/gen/1/ceesafec build --emit c -o <scratchpad>/fpr/a/gen/c2   (in ceesafe)
    $ <scratchpad>/fpr/a/gen/1/ceesafec build --cc cc -o <scratchpad>/fpr/a/gen/2/ceesafec   (in ceesafe)
    $ <scratchpad>/fpr/a/gen/2/ceesafec build --emit c -o <scratchpad>/fpr/a/gen/c3   (in ceesafe)
    $ <scratchpad>/fpr/a/gen/2/ceesafec build --cc cc -o <scratchpad>/fpr/a/gen/3/ceesafec   (in ceesafe)
    $ <scratchpad>/fpr/a/gen/3/ceesafec build --emit c -o <scratchpad>/fpr/a/gen/c4   (in ceesafe)
    $ <scratchpad>/stage0-ceesafec build --cc cc -o <scratchpad>/fpr/deeper/second-run/b/gen/1/ceesafec   (in ceesafe)
    $ <scratchpad>/fpr/deeper/second-run/b/gen/1/ceesafec build --emit c -o <scratchpad>/fpr/deeper/second-run/b/gen/c2   (in ceesafe)
    $ <scratchpad>/fpr/deeper/second-run/b/gen/1/ceesafec build --cc cc -o <scratchpad>/fpr/deeper/second-run/b/gen/2/ceesafec   (in ceesafe)
    $ <scratchpad>/fpr/deeper/second-run/b/gen/2/ceesafec build --emit c -o <scratchpad>/fpr/deeper/second-run/b/gen/c3   (in ceesafe)
    $ <scratchpad>/fpr/deeper/second-run/b/gen/2/ceesafec build --cc cc -o <scratchpad>/fpr/deeper/second-run/b/gen/3/ceesafec   (in ceesafe)
    $ <scratchpad>/fpr/deeper/second-run/b/gen/3/ceesafec build --emit c -o <scratchpad>/fpr/deeper/second-run/b/gen/c4   (in ceesafe)
