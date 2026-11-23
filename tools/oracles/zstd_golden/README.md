# zstd's golden decompression files

Copied unchanged from Zstandard 1.5.7 (https://github.com/facebook/zstd, tag `v1.5.7`): `tests/golden-decompression/`
here, frames every decoder must read, and `tests/golden-decompression-errors/` in `errors/`, frames every decoder must
refuse. They hold corner cases libzstd's own encoder never writes (ADR 0196 §4.2 and question 7, accepted). They are
Zstandard's, under its BSD licence, in `LICENSE` beside them.

`tools/oracles/gen_zstd_cases.py` puts the small ones in `tests/reference/tests/corpus/run-pass/zstd.cee`, and
`tools/codec_check.py` decodes all of them, whole and in steps, with tools/oracles/zstd_model.py's verdicts.
