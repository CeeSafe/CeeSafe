# ADR 0196 — Compression codecs in lib/std: deflate, zlib and gzip; snappy; zstd

- Status: **Accepted** by the owner, 2026-11-21 ("Accept all as recommended": every question as §5 or §6 recommends; release order: a patch first, then the next minor). Proposed the same day. Raised by CosmoStore's CEESAFE_GAPS G35 and G40
  (`~/dev/CeeSafeApps/cosmostore/CEESAFE_GAPS.md`): its Iceberg table maintenance (CosmoStore ADR-0086) reads
  Avro and Parquet that other writers compressed, its compaction (ADR-0098) was designed around never
  decompressing a page, and its inventory reports (ADR-0074) are gzip files of stored blocks. CosmoStore calls no C. Phase 1 implemented for 0.90.0: deflate, zlib and gzip both ways, `Inflater`, `Deflater`, `Adler32`.
- Scope: `lib/std` (new `compress_deflate.cee`, `compress_snappy.cee`, `compress_zstd.cee`; `Adler32`
  and `Xxh64` added to `checksums.cee`; `Crc32` is ADR 0194's, their tables by `tools/gen_checksums.py`), `spec/api/std.md`, the guide's
  built-in types page and the book's Appendix on lib/std, `tools/oracles/`, the corpus, a fuzzer under `tests/fuzz/`,
  `benchmarks/`. No language, checker, emitter, runtime or interpreter change.

## 1. Context

- **What lib/std has.** Checksums and digests (`Crc32c`, `Crc64Nvme`, `Md5`, `Sha256`, `HmacSha256`; ADR 0075 §2.5,
  ADR 0089) and `Aes256Gcm` (ADR 0135). Nothing compresses, and there is no IEEE CRC-32 (gzip's, zip's, PNG's, and
  the one after each of Avro's snappy blocks) and no Adler-32 (zlib's).
- **What the formats CosmoStore must read use** (G40):
  - Parquet pages: `ZSTD` (PyIceberg's default, and Iceberg's `write.parquet.compression-codec` since 1.4),
    `SNAPPY` (Spark's `spark.sql.parquet.compression.codec` default), `GZIP` (RFC 1952 members). Each page is one
    buffer whose decompressed size the page header states.
  - Avro blocks: `deflate` (raw RFC 1951; Iceberg's manifests by default), `snappy` (a raw snappy block and a
    big-endian CRC-32 of the decompressed bytes), `zstandard`.
  - Inventory reports and `Content-Encoding`: gzip.
- **Three copies of deflate already exist in CeeSafe code, all bit-at-a-time:**
  - CosmoStore's `src/s3/inflate.cee` (262 lines; stored, fixed and dynamic blocks, an output limit; zlib's `puff`
    algorithm; tested against zlib's streams);
  - CeeSafe.Web's `web_ws` `inflate.cee` (363 lines) and `deflate.cee` (140 lines: one fixed-Huffman block per
    message, hash chains, the sync flush's tail removed), for permessage-deflate only;
  - httpserver's `src/gzip.cee`, gzip framing and its own CRC-32 tables around `web_ws`'s `Deflater`, where
    deflate's inner loops are the cost ADR 0186 §5 measured (`/gz/16384`).
- **Measured** (2026-11-21, M1, macOS, load average 3 to 5 from other sessions; ceesafec 0.84.1; a 32 MiB
  inventory-shaped CSV with a random 128-bit hex column, 2.3x under zlib level 6; best of three):

  | | decompress | compress |
  |---|---:|---:|
  | zlib 1.2.12 (Python 3.9's `zlib`), level 6; level 1 | 607 MB/s | 38; 133 MB/s |
  | zstd 1.5.7 (`zstd -b`), level 3; level 1 | 1,311; 1,452 MB/s | 530; 1,039 MB/s |
  | CosmoStore's `inflate.cee`, compiled (`ceesafec build`) | **136 MB/s** | — |
  | the same, `ceesafec run --interp`, 256 KiB | about 28 KB/s (4,500x slower) | — |

  So decoding a symbol a bit at a time costs 4.5x against zlib; a storage server that moves GB/s needs table-driven
  decoding, as zlib, libdeflate and zstd do.
- **Why not C.** CeeSafe's runtime C is for CPU instructions a library cannot reach (ADR 0089, ADR 0135), and its
  policy is no third-party C. Linking zlib or libzstd (as the bun port's `bun_zstd` did through ADR 0090) puts a C
  toolchain and a licence under every program that reads a gzip file, and none of these codecs has a hot path that is
  one instruction: they are bit buffers, table lookups and copies, which CeeSafe compiles to the same C.

## 2. Decision

### 2.1 Pure CeeSafe, one shared shape

Every codec is CeeSafe in lib/std, with no `extern` and no `unsafe`. Each has whole-buffer functions and, where the
format streams, an encoder and a decoder type. One error enum serves all decoders:

```
/// Why compressed bytes did not decode.
public enum DecodeError {
    Malformed,                      // not a valid stream, cut short, or bytes after its end
    TooLarge,                       // more output than the limit, or a zstd window above the window limit
    Unsupported,                    // valid but not handled: a zstd dictionary, a zlib preset dictionary
    ChecksumMismatch,               // gzip's CRC-32, zlib's Adler-32, zstd's XXH64 or a length disagrees
    @from OutOfMemory(AllocError),
}
```

### 2.2 Whole buffers

```
public struct Deflate { … }   // raw RFC 1951 (Avro's deflate)
public struct Zlib { … }      // RFC 1950
public struct Gzip { … }      // RFC 1952; decoding takes every member in turn
public struct Snappy { … }    // the raw block format (Parquet, Avro)
public struct Zstd { … }      // RFC 8878 frames; skippable frames are skipped

// on each:
public fn decompress(input: Slice<u8>, limit: usize, out: borrow mut Vector<u8>) -> Result<void, DecodeError>;
public fn compress(input: Slice<u8>, level: u32, out: borrow mut Vector<u8>) -> Result<void, AllocError>;
```

- **The limit is not optional.** No overload decodes without one: the caller states the most it will accept
  (Parquet's header gives the page's size exactly; Avro's reader its block bound). Output past it is `TooLarge`,
  found before it is written, so a decompression bomb costs at most `limit` bytes of memory. Snappy and zstd state
  the size up front (snappy's preamble, zstd's `Frame_Content_Size`): a stated size above the limit is refused
  before anything is allocated, and output that disagrees with it is `Malformed`.
- **`out` is appended to,** as `Vector<u8>` is everywhere in lib/std; on an error its new bytes are removed.
- **All of the input is one stream** (or members, or frames). Bytes after it are `Malformed`, so a truncated or
  padded object is never half-accepted. A caller with data after a raw deflate stream uses the stream type (§2.3),
  which reports how much it read.
- **Levels:** deflate's 0 (stored blocks) to 9, as zlib's in meaning: 1 to 3 a single hash probe and greedy
  matching, 4 to 9 hash chains, lazy matching and longer searches; 6 is the default the documentation names.
  Snappy has one level (the argument is ignored and documented so). Zstd 1 to 3 (phase 4). A level above the range is
  its highest.
- **Snappy `length(input) -> usize?`** reads the preamble alone, so a reader can size a buffer or refuse early.

### 2.3 Streams

```
public enum DeflateFormat { Raw, Zlib, Gzip }
public struct Progress { public read: usize; public written: usize; public done: bool; }

public struct Inflater { … }   // a 32 KiB window and the decoding tables, allocated once
impl Inflater {
    public fn create(format: DeflateFormat, limit: u64) -> Result<Inflater, AllocError>;
    public fn setWindow(borrow mut this, history: Slice<u8>);                       // a preset dictionary
    @effects()
    public fn step(borrow mut this, input: Slice<u8>, out: SliceMut<u8>) -> Result<Progress, DecodeError>;
}
public struct Deflater { … }
impl Deflater {
    public fn create(format: DeflateFormat, level: u32) -> Result<Deflater, AllocError>;
    public fn update(borrow mut this, input: Slice<u8>, out: borrow mut Vector<u8>) -> Result<void, AllocError>;
    public fn flush(borrow mut this, out: borrow mut Vector<u8>) -> Result<void, AllocError>;  // a sync flush
    public fn finish(borrow mut this, out: borrow mut Vector<u8>) -> Result<void, AllocError>;
}
public struct ZstdDecoder { … }  // create(limit: u64, windowLimit: usize), step as Inflater's
```

- **A decoder writes into the caller's slice and allocates nothing after `create`:** `step` is `@effects()`, so
  async code and a `drop` block may call it, and its memory is fixed (32 KiB for deflate; the frame's window,
  refused above `windowLimit`, for zstd: a frame may ask for up to 3.75 TiB, and libzstd's own default limit is
  2^27, 128 MiB, which this takes as its documented suggestion). `done` is true at the stream's end; `read` then
  says where the next data begins. The total `limit` still applies across steps.
- **CPU time is the caller's to bound.** A step does work in proportion to its slices, so async code decodes 64 KiB at
  a time (ADR 0186's bound on running without waiting) or hands a whole buffer to `Async.blocking`; the guide says so.
- **Encoders append** (`@effects(alloc)`): deflate's output is bounded by its input plus 5 bytes per 64 KiB block and
  the framing, so an append cannot run away. `flush` and `setWindow` are what `web_ws` needs (RFC 7692's sync flush,
  context takeover), so CeeSafe.Web may drop its copies in its own time.
- **Snappy's raw format does not stream** (one length, references over the whole block); its framing format
  (`sNaPpY` chunks with masked CRC-32C) is not in this ADR. **A zstd encoder stream** comes with phase 4.

### 2.4 Checksums beside them (`checksums.cee`)

`Adler32` and `Xxh64` (with a seed), each with `create`/`update`/`finish`/`of` as `Crc32c`. gzip's IEEE `Crc32`
(reflected 0xEDB88320; `Crc32.of("123456789".bytes())` is 0xCBF43926) is ADR 0194's, on the CPU's instructions with
a table fallback. This ADR uses it, and phase 1 needs 0194's `Crc32` first.

### 2.5 Decoders of untrusted bytes

- **No input panics a decoder,** neither by an index nor an overflow: every refusal is a `DecodeError`. The fuzzer
  (§4.3) holds this; a panic it finds is a release blocker.
- **Where RFC 1951 leaves room, zlib decides:** incomplete Huffman codes are refused except a single one-bit
  distance code; distance codes 30 and 31 and length codes 286 and 287 are `Malformed`; a distance before the
  window or the stream's start is `Malformed` (no preset zeros). zlib is the oracle and the reader every writer is
  tested against.
- **Not constant time.** Compression leaks content through length (CRIME, BREACH); the documentation says not to
  compress a secret with bytes an attacker chooses.

### 2.6 The order, a release each

1. **Deflate, zlib and gzip, both ways,** with `Adler32` (and ADR 0194's `Crc32`): what CosmoStore uses today (Avro manifests,
   inventory), what httpserver and `web_ws` do by hand, and the base the others' tests reuse.
2. **Snappy, both ways:** small, and Spark's default.
3. **The zstd decoder,** with `Xxh64`: PyIceberg's and Iceberg's default, the largest gap for reading.
4. **A zstd encoder at levels 1 to 3** (zstd's `fast` and `dfast` strategies), later: only re-encoding compaction
   needs it, and CosmoStore can write snappy or gzip pages until then (Question 4).

## 3. Consequences

- **CosmoStore** deletes `src/s3/inflate.cee`, `gzipStored` and `crc32Ieee` (`src/s3/inventory.cee:236`, `:250`);
  reads every codec Iceberg's writers use; writes inventory reports at a third of their size; may re-encode Parquet.
- **lib/std about doubles** (8,756 lines today, §5's sizes add 6,000 to 8,000 in all). `ceesafec check` of a hello
  world takes 0.06 s today; each phase measures it again, and a regression there is a finding of that phase.
- **Speed is a target, not a promise.** Table-driven decoding in CeeSafe is not measured yet; bounds checks in the
  inner loops are the risk. The fast loops are written as libdeflate's: decode while at least 8 input bytes and 258
  output bytes remain, so the slow checks run at a block's edges. A check the compiler cannot remove and costs more
  than 10% is recorded as a gap of the optimiser (ADR 0101's line of work), never worked around with `unsafe`.
- **LZ77 copies overlap.** `copyWithin` is `memmove`, which is wrong for a distance below the length; the codecs copy
  8 bytes at a time with `readLE`/`writeLE` when the distance is 8 or more, and byte by byte below. A lib/core
  primitive is proposed only if a profile shows this copy is the cost.
- **Maintenance:** four codecs of untrusted input in the standard library. The fuzzer and the oracles are part of the
  contract suite, so a later change to the compiler that breaks a codec fails there.

## 4. Implementation and checks

### 4.1 The interpreter

It needs nothing: no `extern` function, so no C library model (unlike ADR 0135 §2.4), and every operation the codecs
use is lib/core's. It is slow (§1: 28 KB/s), so the corpus programs decode and encode a few KiB each; the long lists
run compiled only, as ADR 0135 §5 does for the cipher.

### 4.2 Conformance and oracles (expected values from outside the code)

- **Deflate, zlib, gzip** — oracle: Python's `zlib` (1.2.12 here) and `gzip`, in `tools/oracles/gen_deflate_cases.py`.
  - zlib's streams at every level and strategy (`Z_FILTERED`, `Z_HUFFMAN_ONLY`, `Z_RLE`, `Z_FIXED`), window bits 9
    to 15, input lengths around every block size, and flushes (`Z_SYNC_FLUSH`, `Z_FULL_FLUSH`) mid-stream.
  - Our encoder's output at every level decoded by zlib to the same bytes, with its size reported against zlib's.
  - RFC 1951 edge cases written by hand: LEN/NLEN mismatch, every refusal of §2.5, length 258 by code 284 and 285,
    an empty final block, distances of exactly 32,768. CosmoStore's `inflateRefusals` cases move over.
  - RFC 1952: multi-member files, FEXTRA, FNAME, FCOMMENT and FHCRC built by hand, a wrong CRC or ISIZE refused.
  - RFC 1950: FCHECK and the Adler-32; FDICT refused as `Unsupported` by `Zlib.decompress`.
- **Snappy** — oracle: libsnappy through `ctypes` (Homebrew's snappy is installed here), or `python-snappy` or
  `cramjam` where present, and `tools/oracles/snappy_model.py`, a decoder written from snappy's
  `format_description.txt` that checks itself against the library. Copies of 1, 2 and 4 byte offsets, overlapping
  copies, literals of every length tag, preambles of 1 to 5 bytes, and refusals (an offset of 0 or before the start,
  a length past the preamble).
- **Zstd** — oracle: Python 3.14's `compression.zstd` (PEP 784; libzstd 1.5.7 here), the `zstd` command and the
  `zstandard` module where present. Levels -7 to 22, with and without the content checksum and size, `--long=27`,
  raw and RLE blocks, multiple and skippable frames, the 128 KiB block edge; the decoder's refusals by RFC 8878's
  rules: reserved bits, a window above the limit, an offset before the history, invalid Huffman weights or FSE
  tables, a dictionary ID (`Unsupported`). zstd's own `tests/golden-decompression` files, under its BSD licence with
  its notice, if the owner agrees to vendoring them.
- **Generated, then committed:** the expected bytes live in the corpus and `tests/support`, so CI needs no oracle;
  only regeneration does (AGENTS.md: never regenerate an expectation from CeeSafe).
- **Real files:** Parquet and Avro files written by PyIceberg and Spark, as CosmoStore's tests hold them, decoded page
  by page; CosmoStore's suite is the end-to-end check of each phase.
- **Contract suite:** a step "codecs" (`tools/codec_check.py`), compiled: the long lists, `--check-leaks`, and
  `tools/fault_sweep.py`'s allocation sweep over each `create` and each encoder (every allocation failure is an
  `OutOfMemory`, nothing leaks).

### 4.3 Fuzzing

A mutational fuzzer in CeeSafe for each codec, as CeeSafe.Web's `tests/fuzz/ws` is (seeded, time-bounded by an
environment variable), built with `--cflag -fsanitize=address,undefined`. Seeds: the oracles' streams. Properties:
no panic and no sanitizer report; output never above the limit; a stream decoded in steps of random sizes gives
the bytes of the whole-buffer call; `decompress(compress(x)) == x` for random and structured `x` at every level;
and for deflate, lib/std's `Inflater` against CosmoStore's `puff`-style decoder, a second implementation, on every
mutant: the same bytes or both refuse. Ten minutes per codec before each release; longer runs on the owner's hosts.

### 4.4 Speed

`benchmarks/codecs/`: §1's CSV, a Parquet-like column of integers, and text, against the system's zlib, libzstd
and libsnappy on the same machine; MB/s and size reported in `guide/performance.md`. Targets on the M1, to be
measured, not gates (Question 6): inflate 300 MB/s (half of zlib's 607); deflate level 1 100 MB/s; snappy decode
1,000 MB/s; zstd decode 500 MB/s (two fifths of libzstd's 1,311); zstd level 1 encode 300 MB/s.

## 5. Phases and sizes

Lines are CeeSafe in lib/std, then tests, oracles and fuzzers. References: zlib's inflate is about 2,200 lines of C,
its deflate about 3,400; zstd's educational decoder about 2,300. Days are an agent's working days, with review.

| Phase | Release | Contents | lib/std | tests and tools | Days |
|---|---|---|---:|---:|---:|
| 1 | first minor after acceptance | `Adler32` (with 0194's `Crc32`); `Deflate`/`Zlib`/`Gzip`; `Inflater` (table-driven, streaming, `setWindow`); `Deflater` (levels 0 to 9, dynamic Huffman, length-limited codes, sync flush) | 2,200 to 2,800 | 1,200 | 5 to 7 |
| 2 | next minor | `Snappy` encode and decode, `length` | 400 to 600 | 500 | 1 to 2 |
| 3 | next minor | `Zstd.decompress`, `ZstdDecoder`, `Xxh64`: frames, literals (Huffman, 4 streams), FSE, sequences, repeat offsets | 2,000 to 2,600 | 1,200 | 6 to 9 |
| 4 | later, on request | `Zstd.compress` levels 1 to 3, `ZstdEncoder`: match finders, FSE and Huffman encoders, block splitting | 2,500 to 3,500 | 700 | 8 to 12 |

Each phase is its own branch (`impl/0196-n`), passes the contract suite, the corpus compiled and interpreted, and
CosmoStore's suite with the new API, and is recorded in ROADMAP §10 with the commands and results. Phase 1 also
adds, in CosmoStore's repository, the change that drops its copies; CeeSafe.Web's is Web's to make.

## 6. Questions for the owner

1. **Where:** in lib/std as new files beside `checksums.cee` (recommended: hosted, like the rest; one import-free
   name for every program), or a separate `lib/compress` package a program names in its manifest?
   **Recommended:** lib/std. The kernel profiles get them later if a kernel asks; the code is allocation-free after
   `create` and would move.
2. **Pure CeeSafe for the codecs and their checksums:** settled by ADR 0194, whose `Crc32` uses the CPU's instruction
   where it has one and tables elsewhere. The rest here is pure CeeSafe, with no runtime change.
3. **The API:** both whole-buffer functions and streaming types, with a limit every decoder requires (recommended),
   or whole buffers only, for a smaller first phase?
   **Recommended:** both. Parquet wants whole buffers; gzip responses, inventory writing and `web_ws` want streams,
   and only a stream bounds memory for an input of unknown size.
4. **The zstd encoder:** phases 1 to 3 committed now and phase 4 proposed but started only when CosmoStore schedules
   re-encoding compaction (recommended), or all four committed in order?
   **Recommended:** the former; until then, CosmoStore writes snappy or gzip pages, which every Parquet reader takes.
5. **Out of scope:** LZ4 (Parquet's `LZ4_RAW`), Brotli, xz, bzip2, snappy's framing format and zstd dictionaries,
   each a later ADR when a program asks (recommended), or LZ4_RAW in phase 2 (about 400 lines more)?
   **Recommended:** out; no CeeSafe program has asked for one.
6. **Speed:** correctness gates each release and §4.4's targets are reported (recommended), or a target missed by
   more than a quarter holds the release?
   **Recommended:** report. A slow correct decoder still unblocks CosmoStore (today's runs at 136 MB/s), and a
   miss is recorded as an optimiser gap to work on, not a reason to ship nothing.
7. **zstd's golden test files:** vendor them under BSD with zstd's notice (recommended), or only cases generated
   from libzstd?
   **Recommended:** vendor them; they hold the corner cases libzstd's own encoder never writes.

## As built (0.90.0, phase 1)

- **`Inflater`** is table-driven, from zlib's `inflate_table`. It has a fast loop and a slow path that stops and
  resumes between any two bytes. Its memory is 58 KiB: 25 KiB of tables and the 32 KiB window. The `Deflater`
  allocates about 400 KiB.
- **Levels 1 to 3** use one hash probe each and differ in how much of a match is hashed, so their output is close.
  zlib's search 4, 8 and 32 chain entries.
- **Behaviour §2 did not say:**
  - with gzip, a step after `done` that is given input decodes the next member;
  - an empty gzip input is `Malformed`;
  - with `setWindow`, a zlib FDICT stream whose dictionary id differs is `ChecksumMismatch`;
  - `finish` starts a new stream.
- **Measured** (M1, best of 3, MB/s; `guide/performance.md`):
  - inflate 269 to 477, which is 27 to 45 % of zlib's; the 300 MB/s target is missed on the CSV at levels 6 and 9;
  - deflate faster than zlib at level 1 (166 to 138), and 75 to 95 % of it above;
  - output within 0.6 % of zlib's at levels 6 and 9, and up to 4.6 % larger at level 1.
- **A cost of its own:** `ceesafec check` of a hello world went from 0.065 s to 0.096 s. The checker checks all of
  lib/std for every program, so lib/std's size is every program's. Left for an ADR of its own.
- **Tests:**
  - `run-pass/deflate.cee`: 296 lines from Python's `zlib`;
  - `tools/codec_check.py`, the contract suite's "codecs" step: 8,701 zlib streams decoded whole and in steps,
    3,600 of ours read by zlib, and an allocation sweep of 202 points;
  - a fuzzer under `tests/fuzz/deflate`: 44.8 million inputs and 174,711 round trips under ASan and UBSan, none
    failing, with a differential decoder from CosmoStore's.
