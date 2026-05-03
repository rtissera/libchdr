# AVHuff regression corpus

Small, redistributable AVHuff CHDs to verify libchdr's AVHuff decoder
produces byte-identical output to MAME's `chdman`.

The actual `.chd` / `.avi` payloads are git-ignored; pull them with
`./fetch.sh` (needs `curl`, `chdman`, `ffmpeg`).

Sources:

* `regtest/` — verbatim copies of MAME's `regtests/chdman/{input,output}/createld_avi_*`
  files (BSD-3-Clause / GPL-2). Video-only, 624x176, 6 hunks.

* `synth/` — synthesized via `chdman createld` from the regtest input AVIs
  with a 440 Hz sine track muxed in. Two variants:
  - `avhu_only.chd`: `-c avhu`, FLAC audio (chdman 0.264 default)
  - `flac_audio.chd`: `-c flac,avhu`, dual-codec hunks

Run the harness:

    cmake --build build --target chdr-avhuff-regression
    ./build/tests/chdr-avhuff-regression \
        tests/avhuff_corpus/regtest/createld_avi_yuv2_3_frames_no_audio/out.chd \
        tests/avhuff_corpus/regtest/createld_avi_uyvy_3_frames_no_audio/out.chd \
        tests/avhuff_corpus/synth/avhu_only.chd \
        tests/avhuff_corpus/synth/flac_audio.chd

The harness relies on libchdr's built-in CRC16 verification (`VERIFY_BLOCK_CRC=1`,
default). Any byte-level decode error fails the per-hunk CRC and `chd_read`
returns `CHDERR_DECOMPRESSION_ERROR`.
