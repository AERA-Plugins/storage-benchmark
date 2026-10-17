#define _GNU_SOURCE
#include "bench.h"
#include <assert.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int main(void){
  char root[PATH_MAX];const char *home=getenv("HOME");assert(home);
  assert(snprintf(root,sizeof(root),"%s/aera-bench-test-XXXXXX",home)<(int)sizeof(root));assert(mkdtemp(root));
  char sentinel[4096];snprintf(sentinel,sizeof(sentinel),"%s/existing.txt",root);
  FILE *f=fopen(sentinel,"wx");assert(f);fputs("DO NOT MODIFY\n",f);fclose(f);
  struct benchmark b={.mutex=PTHREAD_MUTEX_INITIALIZER};bench_defaults(&b.options);
  assert(!bench_inspect("/dev/null","Invalid",&b.target));
  assert(bench_inspect(root,"Fixture",&b.target));
  snprintf(b.fio,sizeof(b.fio),"/usr/bin/fio");
  b.options.size=9003008;b.options.seconds=1;b.options.tests=0x3f;
  pthread_t worker;assert(!pthread_create(&worker,NULL,bench_run,&b));pthread_join(worker,NULL);
  struct snapshot s;bench_snapshot(&b,&s);
  printf("%s: %s\n",s.stage,s.error);
  for(unsigned i=0;i<6;i++)printf("%s: %.2f MB/s, %.2f IOPS, samples %u\n",bench_names[i],s.results[i].mb_s,s.results[i].iops,s.results[i].samples);
  assert(!s.error[0]&&!s.cleanup[0]&&s.completed==6);
  for(unsigned i=0;i<6;i++)assert(s.results[i].samples==1&&s.results[i].mb_s>0);
  memset(&b.snapshot,0,sizeof(b.snapshot));atomic_store(&b.done,0);atomic_store(&b.cancel,0);
  b.options.seconds=10;assert(!pthread_create(&worker,NULL,bench_run,&b));
  usleep(700000);atomic_store(&b.cancel,1);pthread_join(worker,NULL);bench_snapshot(&b,&s);
  assert(!strcmp(s.stage,"Benchmark cancelled")&&!s.cleanup[0]);
  DIR *d=opendir(root);assert(d);struct dirent *e;unsigned remaining=0;
  while((e=readdir(d)))if(strcmp(e->d_name,".")&&strcmp(e->d_name,"..")){assert(!strcmp(e->d_name,"existing.txt"));remaining++;}closedir(d);assert(remaining==1);
  char text[64]={0};f=fopen(sentinel,"r");assert(f);assert(fgets(text,sizeof(text),f));fclose(f);assert(!strcmp(text,"DO NOT MODIFY\n"));
  assert(!unlink(sentinel));assert(!rmdir(root));puts("PASS: six fio modes, cancellation, owned cleanup, sentinel unchanged, non-directory rejection");
}
