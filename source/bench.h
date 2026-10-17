/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <limits.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>

#define BENCH_TESTS 6
#define BENCH_TARGETS 12
struct target {
  char path[PATH_MAX], name[96], filesystem[48];
  uint64_t free_bytes;
  unsigned long long device, inode;
  int network;
};
struct options {
  uint64_t size;
  unsigned block, seconds, repeats, workers, tests;
  int direct;
};
struct measurement { double mb_s, iops, latency_ms; unsigned samples; int skipped; };
struct snapshot {
  char stage[128], detail[256], error[512], cleanup[256];
  double progress, live_mb_s;
  uint64_t bytes;
  unsigned completed, total;
  struct measurement results[BENCH_TESTS];
};
struct benchmark {
  pthread_mutex_t mutex;
  atomic_int cancel, done;
  struct target target;
  struct options options;
  struct snapshot snapshot;
  char fio[PATH_MAX];
};
extern const char *const bench_names[BENCH_TESTS];
int bench_targets(struct target *targets, unsigned maximum);
int bench_inspect(const char *path, const char *name, struct target *target);
void bench_defaults(struct options *options);
void bench_snapshot(struct benchmark *bench, struct snapshot *snapshot);
void *bench_run(void *opaque);
