# AERA Storage Benchmark

File-based storage benchmarks for AERA Recovery. Test internal storage, mounted
USB/SD storage, and network storage without writing to raw partitions or existing
files. Requires an ARM64 AERA recovery with Host API 2.

## Tests and Settings

- Sequential read/write, 4K random read/write, and parallel 4K read/write.
- Throughput in MB/s, plus IOPS and latency for applicable workloads.
- Configurable test size, duration, passes, parallel workers, block size, and
  direct or buffered I/O.
- Persistent settings and preferred storage, with Reset to defaults.
- Progress and cancellation. Network storage uses sequential full-file transfers;
  disable its write cache and reconnect before measuring it.

Connect SMB or SFTP through AERA's Network Storage before selecting it here.
The plugin supports both the native AERA backend and older rclone-based mounts.
NAS results include the network, protocol, server, and final close/commit time.
Unsupported direct I/O is reported, not silently replaced with a different test.

## Data Handling

Each run creates its own uniquely named `.aera-storage-bench-*` directory and
verifies file identity and an ownership marker. Cleanup removes only that run's
files. It does not format storage, write raw devices, overwrite existing files,
change schedulers, remount storage, or drop the system's caches.

Every run prepares a new test file, including read-only test selections. Normal
writes consume temporary space and cause ordinary flash wear. The plugin keeps
a free-space reserve. Interrupted power, device removal, filesystem failure, or
a forced process kill may leave its temporary directory for manual cleanup.
This is a performance benchmark, not a filesystem repair or data-integrity test.

## Installation

Install AERA Storage Benchmark from AERA's Plugin Manager. Official releases also
provide a signed `.aerap` package for manual installation through recovery Files.

## Build

With an AERA Android source tree and the AERA plugin registry checked out:

```sh
export AERA_TREE=/path/to/AERA_16.0
export AERA_PLUGIN_REGISTRY=/path/to/registry
bash build.sh
```

The script builds ARM64 fio and the controller using AERA's musl toolchain,
then creates `build/runtime.xz` and the installable package. A local build is
unsigned; official signing uses the private release infrastructure and is not
required to build or inspect the source.

## Source and Licenses

The controller is Apache-2.0. The bundled fio 3.43 sources are GPL-2.0 and include
the AERA NAS adapter in `vendor/fio/engines/sync.c`; its patch is also provided
under `source/`. fio's complete upstream source, build system, and license are
included. The jsmn JSON parser is MIT-licensed. Runtime packages include both
dependency license texts. See [UPSTREAM.md](UPSTREAM.md) for pinned versions.
