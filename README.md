# libchdr

libchdr is a standalone library for reading MAME's CHDv1-v5 formats.

The code is based off of MAME's old C codebase which read up to CHDv4 with OS-dependent features removed, and CHDv5 support backported from MAME's current C++ codebase.

## Embedded cue sheets

CHDv5 track metadata has no room for INDEX 02 and later, CATALOG or ISRC, which some Saturn and Dreamcast titles rely on. libchdr reads the original cue sheet when it is stored in the CHD as `CUES` metadata, and exposes those entries through `chd_cd_get_toc()` and `chd_cd_get_index()` (see `include/libchdr/cdrom.h`). `chdman addmeta` writes only to uncompressed CHDs, so add the cue sheet before compressing:

    chdman createcd -c none -i game.cue -o tmp.chd
    chdman addmeta -i tmp.chd --tag CUES --valuefile game.cue
    chdman copy -i tmp.chd -o game.chd

Other readers ignore the tag, so the result is still an ordinary CHDv5 file.

libchdr is licensed under the BSD 3-Clause (see [LICENSE.txt](LICENSE.txt)) and uses third party libraries that are each distributed under their own terms (see each library's license in [deps/](deps/)).
