# Bundled Source Versions

- fio 3.43: https://github.com/axboe/fio
  - Commit: `d2dcd7e053d1902d2420f151c83990e6acdd9f8c`
  - License: GPL-2.0, see `vendor/fio/COPYING`.
  - Local change: `source/fio-nas-engine.patch`, applied to
    `vendor/fio/engines/sync.c`. The NAS adapter accepts only the benchmark's
    pinned private file and valid ownership marker.
- jsmn: https://github.com/zserge/jsmn
  - Commit: `25647e692c7906b96ffd2b05ca54c097948e879c`
  - License: MIT, see `vendor/jsmn/LICENSE`.
