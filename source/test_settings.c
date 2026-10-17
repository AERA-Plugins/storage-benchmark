#define _GNU_SOURCE
#include "settings.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
int main(void){
  char directory[]="/tmp/aera-bench-settings-XXXXXX";assert(mkdtemp(directory));
  assert(!setenv("BENCH_SETTINGS_DIR",directory,1));
  struct options expected,loaded;bench_defaults(&expected);expected.tests=0x15;expected.seconds=20;expected.direct=0;
  expected.size=2000000000;expected.size-=expected.size%4096;
  const char *target="/storage/test\\volume\"label";
  assert(settings_save(&expected,target));
  char path[PATH_MAX]={0};bench_defaults(&loaded);settings_load(&loaded,path);
  assert(!memcmp(&expected,&loaded,sizeof(loaded))&&!strcmp(path,target));
  bench_defaults(&expected);assert(settings_save(&expected,""));
  memset(&loaded,0,sizeof(loaded));settings_load(&loaded,path);assert(!memcmp(&expected,&loaded,sizeof(loaded))&&!path[0]);
  char file[PATH_MAX];snprintf(file,sizeof(file),"%s/preferences.json",directory);
  FILE *f=fopen(file,"w");assert(f);fputs("{\"version\":1,\"size\":1,\"tests\":4294967295}",f);fclose(f);
  bench_defaults(&loaded);settings_load(&loaded,path);assert(!memcmp(&expected,&loaded,sizeof(loaded)));
  f=fopen(file,"w");assert(f);fputs("{broken json",f);fclose(f);
  bench_defaults(&loaded);settings_load(&loaded,path);assert(!memcmp(&expected,&loaded,sizeof(loaded)));
  assert(!unlink(file));assert(!rmdir(directory));puts("PASS: saved options/target round trip, defaults reset, invalid/corrupt settings ignored");
}
