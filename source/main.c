/* SPDX-License-Identifier: Apache-2.0 */
#define _GNU_SOURCE
#include "bench.h"
#include "settings.h"
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

enum { HELLO=1, BEGIN=2, BUTTON=3, COMMIT=4, STATUS=5, CLOSE=7,
  SECTION=8, METRIC=9, UPDATE=10, BACK=11, ACK=64, ACTION=65, LIFE=66 };
enum { HOME=100, STORAGE, SETTINGS, TESTS, REVIEW, RUNNING, RESULTS, STOP,
  REFRESH=120, START, CANCEL, CONTINUE, SIZE, DURATION, REPEATS, WORKERS,
  BLOCK, DIRECT, RESET, SELECT=200, TOGGLE=300 };
enum { PRIMARY=1, DESTRUCTIVE=2, DISABLED=4, GOOD=256, WARNING=512, ACCENT=2048 };
struct packet {uint32_t magic,version,kind,id,value,flags;char title[96],text[1024];};
_Static_assert(sizeof(struct packet)==1144,"API packet mismatch");
static struct benchmark job={.mutex=PTHREAD_MUTEX_INITIALIZER};
static struct target targets[BENCH_TARGETS];
static unsigned target_count,page=HOME;
static int selected,active,resumed,failed_send;
static pthread_t worker;
static struct options result_options;
static struct target result_target;
static int have_results;
static char preferred_target[PATH_MAX];
static int settings_failed;
static double started;
static volatile sig_atomic_t quitting;

static double clock_seconds(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return t.tv_sec+t.tv_nsec/1e9;}
static void on_signal(int signal){(void)signal;quitting=1;}
static void send_packet(unsigned kind,unsigned id,unsigned value,unsigned flags,const char *title,const char *text){
  if(failed_send)return;
  struct packet p={.magic=0x41325049,.version=2,.kind=kind,.id=id,.value=value,.flags=flags};
  snprintf(p.title,sizeof(p.title),"%s",title?title:"");snprintf(p.text,sizeof(p.text),"%s",text?text:"");
  ssize_t n;do{n=send(4,&p,sizeof(p),MSG_NOSIGNAL);}while(n<0&&errno==EINTR&&!quitting);
  if(n!=(ssize_t)sizeof(p))failed_send=1;
}
static void metric(unsigned kind,unsigned id,unsigned flags,const char *title,const char *format,...){
  char text[768];va_list args;va_start(args,format);vsnprintf(text,sizeof(text),format,args);va_end(args);
  send_packet(kind,id,0,flags,title,text);
}
static void button(unsigned id,unsigned flags,const char *title,const char *text){send_packet(BUTTON,id,0,flags,title,text);}
static void section(const char *title){send_packet(SECTION,0,0,0,title,NULL);}
static void begin(const char *title,const char *body,unsigned back){send_packet(BEGIN,0,0,0,title,body);if(back)send_packet(BACK,back,0,0,NULL,NULL);}
static void defaults(void){bench_defaults(&job.options);}
static void refresh(void){
  char previous[PATH_MAX]={0};snprintf(previous,sizeof(previous),"%s",preferred_target);
  target_count=(unsigned)bench_targets(targets,10);selected=0;
#ifdef BENCH_HOST_TEST
  const char *fixture=getenv("BENCH_FIXTURE");
  if(fixture&&bench_inspect(fixture,"Test fixture",&targets[0]))target_count=1;
#endif
  for(unsigned i=0;i<target_count;i++)if(!strcmp(previous,targets[i].path)){job.target=targets[i];selected=1;break;}
  if(!selected&&target_count){job.target=targets[0];selected=1;}
}
static void configuration(void){
  metric(METRIC,1,ACCENT,"Test file","%.0f MB",job.options.size/1000000.0);
  metric(METRIC,2,0,"Duration","%u s / test",job.options.seconds);
  metric(METRIC,3,0,"Passes","%u",job.options.repeats);
  metric(METRIC,4,0,"I/O mode","%s",selected&&job.target.network?"NAS transfer":job.options.direct?"Direct":"Buffered");
}
static void result_rows(unsigned kind){
  struct snapshot s;bench_snapshot(&job,&s);
  for(unsigned i=0;i<BENCH_TESTS;i++){
    struct measurement m=s.results[i];unsigned color=i%2?WARNING:ACCENT;
    if(m.samples)metric(kind,20+i,color,bench_names[i],"%.1f MB/s",m.mb_s/m.samples);
    else metric(kind,20+i,0,bench_names[i],"%s",m.skipped?"Not supported on NAS":job.options.tests&(1U<<i)?"Not measured":"Not selected");
  }
}
static void running_metrics(unsigned kind){
  struct snapshot s;bench_snapshot(&job,&s);
  const char *phase="Starting";
  if(!strncmp(s.stage,"Preparing",9))phase="Prep";
  else if(!strncmp(s.stage,"Cleaning",8))phase="Cleanup";
  else for(unsigned i=0;i<BENCH_TESTS;i++)if(!strcmp(s.stage,bench_names[i])){static const char *short_names[]={"SEQ read","SEQ write","4K read","4K write","4K xN read","4K xN write"};phase=short_names[i];break;}
  metric(kind,1,ACCENT,"Current test","%s",phase);
  metric(kind,2,0,"Progress","%.0f%%",s.progress);
  metric(kind,3,GOOD,"Throughput","%.1f MB/s",s.live_mb_s);
  metric(kind,4,0,"Transferred this test","%.1f MB",s.bytes/1000000.0);
  metric(kind,5,0,"Elapsed","%.0f s",clock_seconds()-started);
  metric(kind,6,0,"Completed tests","%u / %u",s.completed,s.total);
}
static void render(void){
  char text[768];
  switch(page){
    case STORAGE:
      begin("Choose storage","Mounted, writable storage",HOME);
      if(!target_count)metric(METRIC,1,WARNING,"No writable storage","Unlock internal storage or connect USB / Network Storage first.");
      for(unsigned i=0;i<target_count;i++){
        snprintf(text,sizeof(text),"%.400s / %.1f GB free",targets[i].path,targets[i].free_bytes/1e9);
        button(SELECT+i,selected&&!strcmp(job.target.path,targets[i].path)?PRIMARY:0,targets[i].name,text);
      }
      button(REFRESH,0,"Refresh storage",NULL);button(HOME,0,"Done",NULL);break;
    case SETTINGS:
      begin("Test settings","Storage Benchmark",HOME);
      configuration();section("Workload");
      snprintf(text,sizeof(text),"%.0f MB",job.options.size/1000000.0);button(SIZE,0,"Test file size",text);
      snprintf(text,sizeof(text),"%u seconds per test",job.options.seconds);button(DURATION,0,"Duration",text);
      snprintf(text,sizeof(text),"%u",job.options.repeats);button(REPEATS,0,"Passes",text);
      snprintf(text,sizeof(text),"%u workers / queue depth 1 each",job.options.workers);button(WORKERS,0,"Parallel 4K workers",text);
      snprintf(text,sizeof(text),"%u KB",job.options.block/1024);button(BLOCK,0,"Sequential block size",text);
      button(DIRECT,selected&&job.target.network?DISABLED:0,"I/O mode",selected&&job.target.network?"NAS stream / no local write cache":job.options.direct?"Direct I/O / bypass file cache":"Buffered I/O / cache affects results");
      button(RESET,0,"Reset to defaults","All test settings, test selection and storage choice");button(HOME,PRIMARY,"Done",NULL);break;
    case TESTS:
      begin("Select tests","Read and write workloads",HOME);
      section("Sequential / large files");
      for(unsigned i=0;i<BENCH_TESTS;i++){
        if(i==2)section("Random / small blocks");
        if(i==4)section("Parallel / small blocks");
        const int nas=selected&&job.target.network&&i>=2;
        button(TOGGLE+i,nas?DISABLED:(job.options.tests&(1U<<i)?PRIMARY:0),bench_names[i],nas?"Unavailable on NAS":job.options.tests&(1U<<i)?"Selected":"Not selected");
      }button(HOME,0,"Done",NULL);break;
    case REVIEW:
      begin("Ready to benchmark","Review test destination",HOME);
      section("Destination");metric(METRIC,10,ACCENT,"Storage","%s",selected?job.target.name:"No storage selected");
      metric(METRIC,11,0,"Folder","%s",selected?job.target.path:"Choose storage first");
      section("Profile");configuration();
      section("Before starting");
      metric(METRIC,12,WARNING,"File-based test","A new temporary file is written and removed. Existing files and raw partitions are not test targets. Writes cause normal flash wear.");
      if(selected&&job.target.network)metric(METRIC,13,WARNING,"NAS","Sequential tests only. Disable NAS write cache and reconnect first. Results include network and server overhead.");
      else if(!job.options.direct)metric(METRIC,13,WARNING,"Buffered I/O","RAM cache can inflate results. Do not compare buffered and direct results.");
      button(START,selected&&job.options.tests?PRIMARY:DISABLED,"Start benchmark",NULL);button(HOME,0,"Back",NULL);break;
    case RUNNING:
      begin("Benchmark running",selected?job.target.name:"",STOP);
      running_metrics(METRIC);section("Results so far");result_rows(METRIC);
      button(STOP,DESTRUCTIVE,"Cancel benchmark","Stop I/O and remove this run's temporary files");break;
    case STOP:
      begin("Stop benchmark?","The current test result will be incomplete.",CONTINUE);
      button(CONTINUE,PRIMARY,"Continue benchmark",NULL);button(CANCEL,DESTRUCTIVE,"Stop and clean up",NULL);break;
    case RESULTS:{
      struct snapshot s;bench_snapshot(&job,&s);
      struct options current_options=job.options;struct target current_target=job.target;
      if(have_results){job.options=result_options;job.target=result_target;}
      begin("Benchmark results",s.stage[0]?s.stage:"No benchmark yet",HOME);
      if(s.error[0])metric(METRIC,1,WARNING,"Test stopped","%s",s.error);
      if(s.cleanup[0])metric(METRIC,2,WARNING,"Cleanup needed","%s",s.cleanup);
      section("Throughput");result_rows(METRIC);
      section("4K I/O / average latency");
      for(unsigned i=2;i<BENCH_TESTS;i++)if(s.results[i].samples){
        struct measurement m=s.results[i];
        metric(METRIC,30+i,0,bench_names[i],"%.0f IOPS / %.3f ms",m.iops/m.samples,m.latency_ms/m.samples);
      }
      section("Test profile");configuration();
      metric(METRIC,40,0,"Target","%s",selected?job.target.path:"None");
      button(REVIEW,PRIMARY,"Run again",NULL);button(HOME,0,"Done",NULL);
      job.options=current_options;job.target=current_target;break;
    }
    default:
      page=HOME;begin("Storage Benchmark","AERA / storage performance",0);
      section("Destination");
      metric(METRIC,10,ACCENT,"Storage","%s",selected?job.target.name:"Choose storage");
      metric(METRIC,11,0,"Free space","%.1f GB",selected?job.target.free_bytes/1e9:0);
      metric(METRIC,12,0,"Path","%s",selected?job.target.path:"No writable storage found");
      if(selected)metric(METRIC,13,0,"Filesystem","%s",job.target.filesystem);
      if(settings_failed)metric(METRIC,14,WARNING,"Settings not saved","Internal storage must be unlocked and writable.");
      section("Test profile");configuration();
      button(REVIEW,selected?PRIMARY:DISABLED,"Start benchmark",NULL);
      button(STORAGE,0,"Choose storage","Internal / USB / network");
      button(SETTINGS,0,"Test settings","Size, duration, passes and I/O mode");
      button(TESTS,0,"Select tests","Sequential / 4K random / 4K parallel");
      button(RESULTS,0,"Last results",NULL);break;
  }
  send_packet(COMMIT,0,0,0,NULL,NULL);
}
static unsigned cycle(unsigned current,const unsigned *values,unsigned count){for(unsigned i=0;i<count;i++)if(values[i]==current)return values[(i+1)%count];return values[0];}
static void handle(unsigned action){
  if(active){
    if(action==CANCEL){atomic_store(&job.cancel,1);page=RUNNING;render();send_packet(STATUS,0,0,0,NULL,"Stopping and cleaning up...");}
    else if(action==STOP||action==CONTINUE){page=action==STOP?STOP:RUNNING;render();}
    return;
  }
  int changed=0;
  if(action>=SELECT&&action<SELECT+target_count){job.target=targets[action-SELECT];selected=1;page=HOME;snprintf(preferred_target,sizeof(preferred_target),"%s",job.target.path);changed=1;}
  else if(action>=TOGGLE&&action<TOGGLE+BENCH_TESTS){job.options.tests^=1U<<(action-TOGGLE);page=TESTS;changed=1;}
  else switch(action){
    case REFRESH:refresh();page=STORAGE;break;
    case START:
      if(!selected||!job.options.tests)return;
      memset(&job.snapshot,0,sizeof(job.snapshot));atomic_store(&job.cancel,0);atomic_store(&job.done,0);
      result_options=job.options;result_target=job.target;have_results=1;
      started=clock_seconds();page=RUNNING;active=1;
      if(pthread_create(&worker,NULL,bench_run,&job)){active=0;page=RESULTS;snprintf(job.snapshot.error,sizeof(job.snapshot.error),"Could not start the test worker.");}
      break;
    case SIZE:{const unsigned v[]={128,256,512,1000,2000,4000};unsigned mb=(unsigned)((job.options.size+4095)/1000000);if(mb==127||mb==255||mb==511||mb==999||mb==1999||mb==3999)mb++;
      job.options.size=(uint64_t)cycle(mb,v,6)*1000000;job.options.size-=job.options.size%4096;break;}
    case DURATION:{const unsigned v[]={5,10,20,30};job.options.seconds=cycle(job.options.seconds,v,4);break;}
    case REPEATS:job.options.repeats=job.options.repeats%3+1;break;
    case WORKERS:{const unsigned v[]={1,2,4};job.options.workers=cycle(job.options.workers,v,3);break;}
    case BLOCK:{const unsigned v[]={262144,1048576,4194304};job.options.block=cycle(job.options.block,v,3);break;}
    case DIRECT:job.options.direct=!job.options.direct;break;
    case RESET:defaults();preferred_target[0]=0;refresh();break;
    default:if(action>=HOME&&action<=RESULTS)page=action;else return;
  }
  if(changed||(action>=SIZE&&action<=RESET))settings_failed=!settings_save(&job.options,preferred_target);
  render();
}
int main(void){
  signal(SIGTERM,on_signal);signal(SIGINT,on_signal);signal(SIGPIPE,SIG_IGN);
  defaults();settings_load(&job.options,preferred_target);const char *root=getenv("AERA_PLUGIN_ROOT");
  if(!root||snprintf(job.fio,sizeof(job.fio),"%s/usr/bin/fio",root)>=(int)sizeof(job.fio)||access(job.fio,X_OK))return 78;
  send_packet(HELLO,0,2,2,NULL,"AERA Storage Benchmark");
  int initialized=0;
  while(!quitting&&!failed_send){
    struct pollfd p={4,POLLIN,0};int ready=poll(&p,1,500);
    if(ready<0){if(errno==EINTR)continue;break;}
    if(ready>0){
      if(!(p.revents&POLLIN))break;
      struct packet m;ssize_t count=recv(4,&m,sizeof(m),MSG_TRUNC);
      if(count!=sizeof(m)||m.magic!=0x41325049||m.version!=2||!memchr(m.title,0,sizeof(m.title))||!memchr(m.text,0,sizeof(m.text)))break;
      if(m.kind==ACK&&!(m.flags&1))break;
      if(m.kind==CLOSE||(m.kind==LIFE&&m.value==3))break;
      if(m.kind==LIFE){resumed=m.value==1;if(resumed){if(!initialized)refresh();render();initialized=1;}}
      if(m.kind==ACTION&&initialized)handle(m.id);
    }
    if(active&&atomic_load(&job.done)){pthread_join(worker,NULL);active=0;page=RESULTS;if(resumed)render();}
    else if(active&&resumed&&page==RUNNING)running_metrics(UPDATE);
  }
  if(active){atomic_store(&job.cancel,1);pthread_join(worker,NULL);}
  pthread_mutex_destroy(&job.mutex);return failed_send?78:0;
}
