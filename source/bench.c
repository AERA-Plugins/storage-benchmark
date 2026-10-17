/* SPDX-License-Identifier: Apache-2.0 */
#define _GNU_SOURCE
#include "bench.h"
#define JSMN_STATIC
#include "../vendor/jsmn/jsmn.h"
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <mntent.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/file.h>
#include <sys/statfs.h>
#include <sys/statvfs.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

const char *const bench_names[BENCH_TESTS] = {"Sequential read", "Sequential write",
  "4K random read", "4K random write", "4K parallel read", "4K parallel write"};
static const char marker[] = "AERA-STORAGE-BENCH-1\n";
static const char prefix[] = ".aera-storage-bench-";

static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec+t.tv_nsec/1e9; }
static int is_under(const char *path, const char *root) {
  const size_t n=strlen(root); return !strncmp(path,root,n)&&(!path[n]||path[n]=='/');
}
int bench_inspect(const char *path, const char *name, struct target *target) {
  char real[PATH_MAX]; struct stat st, root; struct statfs fs; struct statvfs space;
  if(!realpath(path,real)||stat(real,&st)||!S_ISDIR(st.st_mode)||stat("/",&root)||
     statfs(real,&fs)||statvfs(real,&space)||
     (space.f_flag&ST_RDONLY)||access(real,R_OK|W_OK))return 0;
#ifndef BENCH_HOST_TEST
  if(st.st_dev==root.st_dev)return 0;
  if(!(is_under(real,"/data/media")||is_under(real,"/mnt/nas")||
       is_under(real,"/usb_otg")||is_under(real,"/usb-otg")||
       is_under(real,"/external_sd")||is_under(real,"/storage")))return 0;
#endif
  if(fs.f_type==0x01021994||fs.f_type==0x858458f6||fs.f_type==0x9fa0||fs.f_type==0x62656572)return 0;
  memset(target,0,sizeof(*target));
  snprintf(target->path,sizeof(target->path),"%s",real);
  snprintf(target->name,sizeof(target->name),"%s",name);
  target->device=st.st_dev;target->inode=st.st_ino;
  target->free_bytes=(uint64_t)space.f_bavail*space.f_frsize;
  target->network=is_under(real,"/mnt/nas")||fs.f_type==0x6969||fs.f_type==0x517b;
  FILE *mounts=setmntent("/proc/mounts","r");
  if(mounts){size_t best=0;struct mntent *entry;
    while((entry=getmntent(mounts)))if(is_under(real,entry->mnt_dir)&&strlen(entry->mnt_dir)>best){
      best=strlen(entry->mnt_dir);snprintf(target->filesystem,sizeof(target->filesystem),"%s",entry->mnt_type);
    }endmntent(mounts);
  }
  if(!target->filesystem[0])snprintf(target->filesystem,sizeof(target->filesystem),"0x%lx",(unsigned long)fs.f_type);
  return 1;
}
int bench_targets(struct target *targets,unsigned maximum) {
  unsigned count=0;const char *paths[]={"/data/media/0","/sdcard","/mnt/nas","/usb_otg","/usb-otg","/external_sd"};
  for(unsigned i=0;i<sizeof(paths)/sizeof(paths[0])&&count<maximum;i++){
    struct target target;
    if(!bench_inspect(paths[i],i<2?"Internal storage":i==2?"Network storage":"USB / SD storage",&target))continue;
    unsigned j;for(j=0;j<count;j++)if(!strcmp(target.path,targets[j].path))break;
    if(j==count)targets[count++]=target;
  }
  FILE *mounts=setmntent("/proc/mounts","r");
  if(mounts){struct mntent *entry;
    while(count<maximum&&(entry=getmntent(mounts))){
      if(!(is_under(entry->mnt_dir,"/storage")||is_under(entry->mnt_dir,"/usb_otg")||is_under(entry->mnt_dir,"/usb-otg")))continue;
      struct target target;if(!bench_inspect(entry->mnt_dir,"Removable storage",&target))continue;
      unsigned j;for(j=0;j<count;j++)if(!strcmp(target.path,targets[j].path))break;
      if(j==count)targets[count++]=target;
    }endmntent(mounts);
  }return (int)count;
}
void bench_defaults(struct options *o) {
  *o=(struct options){512000000,1048576,5,1,4,0x0f,1};
  o->size-=o->size%4096;
}
void bench_snapshot(struct benchmark *b,struct snapshot *s){pthread_mutex_lock(&b->mutex);*s=b->snapshot;pthread_mutex_unlock(&b->mutex);}
static void stage(struct benchmark *b,const char *title,const char *detail){
  pthread_mutex_lock(&b->mutex);snprintf(b->snapshot.stage,sizeof(b->snapshot.stage),"%s",title);
  snprintf(b->snapshot.detail,sizeof(b->snapshot.detail),"%.255s",detail?detail:"");pthread_mutex_unlock(&b->mutex);
}
static void fail(struct benchmark *b,const char *text){
  pthread_mutex_lock(&b->mutex);snprintf(b->snapshot.error,sizeof(b->snapshot.error),"%s",text);pthread_mutex_unlock(&b->mutex);
}
static int json_field(const char *s,const jsmntok_t *t,int count,int object,const char *key){
  if(object<0||t[object].type!=JSMN_OBJECT)return -1;
  for(int i=object+1;i<count&&t[i].start<t[object].end;){
    const int value=i+1;
    if(t[i].type==JSMN_STRING&&(int)strlen(key)==t[i].end-t[i].start&&!strncmp(s+t[i].start,key,strlen(key)))return value;
    i=value+1;while(i<count&&t[i].start<t[value].end)i++;
  }return -1;
}
static double json_number(const char *s,const jsmntok_t *t,int i){return i<0?0:strtod(s+t[i].start,NULL);}
static int parse_json(const char *s,size_t length,int writing,struct measurement *m,uint64_t *bytes,int *fio_error,size_t *consumed){
  jsmntok_t *tokens=malloc(8192*sizeof(*tokens));if(!tokens)return -1;
  jsmn_parser parser;jsmn_init(&parser);
  const int count=jsmn_parse(&parser,s,length,tokens,8192);
  if(count==JSMN_ERROR_PART){free(tokens);return 0;}
  if(count<1||tokens[0].type!=JSMN_OBJECT){free(tokens);return -1;}
  *consumed=(size_t)tokens[0].end;
  const int jobs=json_field(s,tokens,count,0,"jobs");
  if(jobs<0||tokens[jobs].type!=JSMN_ARRAY||tokens[jobs].size<1){free(tokens);return -1;}
  const int job=jobs+1;
  *fio_error=(int)json_number(s,tokens,json_field(s,tokens,count,job,"error"));
  const int io=json_field(s,tokens,count,job,writing?"write":"read");if(io<0){free(tokens);return -1;}
  memset(m,0,sizeof(*m));
  m->mb_s=json_number(s,tokens,json_field(s,tokens,count,io,"bw_bytes"))/1000000.0;
  m->iops=json_number(s,tokens,json_field(s,tokens,count,io,"iops"));
  const int latency=json_field(s,tokens,count,io,"clat_ns");
  m->latency_ms=json_number(s,tokens,json_field(s,tokens,count,latency,"mean"))/1000000.0;
  *bytes=(uint64_t)json_number(s,tokens,json_field(s,tokens,count,io,"io_bytes"));
  free(tokens);
  return isfinite(m->mb_s)&&isfinite(m->iops)&&isfinite(m->latency_ms)?1:-1;
}
static int run_fio(struct benchmark *b,int directory,int test,int prepare,struct measurement *measurement){
  char filename[96],size[48],block[48],seconds[32],workers[24],direct[24];
  snprintf(filename,sizeof(filename),"--filename=/proc/self/fd/6/data.bin");
  const uint64_t effective_size=b->options.size/b->options.block*b->options.block;
  snprintf(size,sizeof(size),"--size=%llu",(unsigned long long)effective_size);
  snprintf(block,sizeof(block),"--bs=%u",test<2||prepare?b->options.block:4096);
  snprintf(seconds,sizeof(seconds),"--runtime=%u",b->options.seconds);
  snprintf(workers,sizeof(workers),"--numjobs=%u",test>=4?b->options.workers:1);
  snprintf(direct,sizeof(direct),"--direct=%d",b->target.network?0:!!b->options.direct);
  const int writing=prepare||(test%2);const char *rw=prepare||test==1?"--rw=write":test==0?"--rw=read":writing?"--rw=randwrite":"--rw=randread";
  const int timed=!prepare&&!b->target.network;
  char *args[]={b->fio,"--name=aera-storage-benchmark",filename,size,block,workers,direct,(char *)rw,
    b->target.network?"--ioengine=aera_nas":"--ioengine=psync","--thread=1","--group_reporting=1","--allow_file_create=0",
    b->target.network?"--create_on_open=1":"--create_on_open=0",
    "--fallocate=none","--unlink=0","--disk_util=0","--eta=never","--output-format=json",
    prepare&&!b->target.network?"--file_append=1":"--file_append=0",
    "--status-interval=1","--invalidate=1","--refill_buffers=1","--randrepeat=0",
    "--exitall_on_error=1",writing?"--end_fsync=1":"--end_fsync=0",timed?seconds:"--loops=1",timed?"--time_based=1":"--time_based=0",NULL};
  int channels[2];if(pipe2(channels,O_CLOEXEC))return 0;
  const int safe_dir=fcntl(directory,F_DUPFD_CLOEXEC,20),safe_out=fcntl(channels[1],F_DUPFD_CLOEXEC,20);
  if(safe_dir<0||safe_out<0){if(safe_dir>=0)close(safe_dir);if(safe_out>=0)close(safe_out);close(channels[0]);close(channels[1]);return 0;}
  const pid_t child=fork();
  if(child==0){
    close(channels[0]);close(channels[1]);close(4);
    if(dup2(safe_dir,6)<0||dup2(safe_out,STDOUT_FILENO)<0||dup2(safe_out,STDERR_FILENO)<0)_exit(127);
    close(safe_dir);close(safe_out);
    execv(b->fio,args);_exit(127);
  }
  close(safe_dir);close(safe_out);close(channels[1]);if(child<0){close(channels[0]);return 0;}
  fcntl(channels[0],F_SETFL,O_NONBLOCK);
  char *buffer=malloc(524288);size_t used=0;int status=0,done=0,terminated=0,have=0,error=0;
  double stopped=0,started=now();struct measurement last={0};
  if(!buffer){kill(child,SIGKILL);waitpid(child,NULL,0);close(channels[0]);return 0;}
  int log=open("/tmp/aera-storage-benchmark-fio.log",O_WRONLY|O_CREAT|O_TRUNC|O_CLOEXEC|O_NOFOLLOW,0600);
  size_t logged=0;char hint[192]={0};
  for(;;){
    if((atomic_load(&b->cancel)||now()-started>300)&&!terminated){kill(child,SIGTERM);terminated=1;stopped=now();}
    if(terminated&&!done&&now()-stopped>2)kill(child,SIGKILL);
    struct pollfd pollfd={channels[0],POLLIN,0};poll(&pollfd,1,100);
    ssize_t got=read(channels[0],buffer+used,524287-used);
    if(got>0){
      if(log>=0&&logged<65536){size_t amount=(size_t)got;if(amount>65536-logged)amount=65536-logged;ssize_t written=write(log,buffer+used,amount);if(written>0)logged+=(size_t)written;}
#if defined(BENCH_HOST_TEST) || defined(BENCH_DIAGNOSTIC)
      if(getenv("BENCH_TRACE"))write(STDERR_FILENO,buffer+used,(size_t)got);
#endif
      used+=(size_t)got;buffer[used]=0;
      while(used){
        char *start=memchr(buffer,'{',used);
        if(!start){if(used>8192){memmove(buffer,buffer+used-4096,4096);used=4096;}break;}
        if(start>buffer){const size_t skip=(size_t)(start-buffer);if(!hint[0])snprintf(hint,sizeof(hint),"%.*s",(int)(skip<sizeof(hint)?skip:sizeof(hint)-1),buffer);memmove(buffer,start,used-skip);used-=skip;}
        struct measurement current;uint64_t bytes=0;size_t consumed=0;int fio_error=0;
        int result=parse_json(buffer,used,writing,&current,&bytes,&fio_error,&consumed);
        if(!result)break;
        if(result<0){error=EPROTO;break;}
        have=1;last=current;if(fio_error)error=fio_error;
        pthread_mutex_lock(&b->mutex);b->snapshot.live_mb_s=current.mb_s;b->snapshot.bytes=bytes;
        b->snapshot.progress=prepare?fmin(99,100.0*bytes/b->options.size):
          100.0*(b->snapshot.completed+fmin(.99,(now()-started)/b->options.seconds))/b->snapshot.total;
        pthread_mutex_unlock(&b->mutex);
        memmove(buffer,buffer+consumed,used-consumed);used-=consumed;
      }
      if(used>=524287||error){kill(child,SIGTERM);terminated=1;if(!stopped)stopped=now();}
    }
    if(!done){pid_t result=waitpid(child,&status,WNOHANG);if(result==child)done=1;else if(result<0&&errno!=EINTR){error=errno;done=1;}}
    if(done&&got<=0)break;
  }
  close(channels[0]);
  if(log>=0)close(log);
  if(!have||error||terminated||!WIFEXITED(status)||WEXITSTATUS(status)){
    if(!atomic_load(&b->cancel)){
      char message[512];
      if(!hint[0]&&!have&&used)snprintf(hint,sizeof(hint),"%.*s",(int)(used<sizeof(hint)?used:sizeof(hint)-1),buffer);
      snprintf(message,sizeof(message),"%s: fio exit %d, error %d (%s). %.191s%s Log: /tmp/aera-storage-benchmark-fio.log",prepare?"Preparation":bench_names[test],WIFEXITED(status)?WEXITSTATUS(status):-1,error,error?strerror(error):"no I/O error reported",hint,error==EINVAL&&b->options.direct?" Try buffered I/O.":"");
      fail(b,message);
    }free(buffer);return 0;
  }
  if(b->target.network)last.mb_s=effective_size/1000000.0/fmax(.001,now()-started);
  free(buffer);*measurement=last;return 1;
}
static int owned_directory(int fd){
  int marker_fd=openat(fd,"owner",O_RDONLY|O_CLOEXEC|O_NOFOLLOW);char text[sizeof(marker)]={0};
  if(marker_fd<0)return 0;ssize_t bytes=read(marker_fd,text,sizeof(text));close(marker_fd);
  return bytes==(ssize_t)strlen(marker)&&!memcmp(text,marker,strlen(marker));
}
static int remove_owned(int root,const char *name){
  const int directory=openat(root,name,O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW);
  if(directory<0||!owned_directory(directory)){if(directory>=0)close(directory);return 0;}
  DIR *scan=fdopendir(dup(directory));struct dirent *entry;int foreign=0;
  if(!scan){close(directory);return 0;}
  while((entry=readdir(scan)))if(strcmp(entry->d_name,".")&&strcmp(entry->d_name,"..")&&strcmp(entry->d_name,"owner")&&strcmp(entry->d_name,"data.bin"))foreign=1;
  closedir(scan);if(foreign){close(directory);return 0;}
  int okay=1;if(unlinkat(directory,"data.bin",0)&&errno!=ENOENT)okay=0;
  if(okay&&unlinkat(directory,"owner",0))okay=0;close(directory);
  if(okay&&unlinkat(root,name,AT_REMOVEDIR))okay=0;return okay;
}
static int nas_uncached(const struct target *target){
  if(!target->network)return 1;
  if(!strcmp(target->filesystem,"fuse.aera-netfs")){
    char json[4096];FILE *file=fopen("/tmp/aera_nas/status.json","rb");if(!file)return 0;
    size_t size=fread(json,1,sizeof(json)-1,file);int complete=feof(file);fclose(file);json[size]=0;
    if(!complete)return 0;
    jsmn_parser parser;jsmn_init(&parser);jsmntok_t tokens[64];
    int count=jsmn_parse(&parser,json,size,tokens,64);if(count<1)return 0;
    int cache=json_field(json,tokens,count,0,"disk_cache"),connected=json_field(json,tokens,count,0,"connected");
    return cache>=0&&connected>=0&&tokens[cache].type==JSMN_PRIMITIVE&&
      tokens[cache].end-tokens[cache].start==5&&!strncmp(json+tokens[cache].start,"false",5)&&
      tokens[connected].type==JSMN_PRIMITIVE&&tokens[connected].end-tokens[connected].start==4&&
      !strncmp(json+tokens[connected].start,"true",4);
  }
  if(strcmp(target->filesystem,"fuse.rclone")&&strcmp(target->filesystem,"fuse"))return 1;
  DIR *proc=opendir("/proc");if(!proc)return 0;int okay=0;struct dirent *entry;
  while((entry=readdir(proc))){
    if(!isdigit((unsigned char)entry->d_name[0]))continue;
    char path[512],buffer[16384];snprintf(path,sizeof(path),"/proc/%.255s/cmdline",entry->d_name);
    FILE *input=fopen(path,"rb");if(!input)continue;
    size_t size=fread(buffer,1,sizeof(buffer)-1,input);fclose(input);buffer[size]=0;
    char *args[128];unsigned count=0;
    for(size_t offset=0;offset<size&&count<128;){args[count++]=buffer+offset;offset+=strnlen(buffer+offset,size-offset)+1;}
    if(!count||!strstr(args[0],"rclone"))continue;
    int mount=0,off=0,ours=0;
    for(unsigned i=1;i<count;i++){
      if(!strcmp(args[i],"mount"))mount=1;
      if(!strcmp(args[i],"--vfs-cache-mode=off")||(!strcmp(args[i],"--vfs-cache-mode")&&i+1<count&&!strcmp(args[i+1],"off")))off=1;
      if(args[i][0]=='/'&&is_under(target->path,args[i]))ours=1;
    }
    if(mount&&ours&&off){okay=1;break;}
  }closedir(proc);return okay;
}
void *bench_run(void *opaque){
  struct benchmark *b=opaque;int root=-1,directory=-1,file=-1,lock=-1;char name[64]={0};struct target current;
  lock=open("/tmp/aera-storage-benchmark.lock",O_RDWR|O_CREAT|O_CLOEXEC|O_NOFOLLOW,0600);
  if(lock<0||flock(lock,LOCK_EX|LOCK_NB)){fail(b,"Another storage benchmark is already running, or the run lock is unavailable.");goto finish;}
  if(!b->options.tests||(b->options.tests&~0x3fU)||b->options.size<b->options.block||b->options.size%4096||b->options.size>4000000000ULL||
    b->options.seconds<1||b->options.seconds>60||b->options.repeats<1||b->options.repeats>3||
    (b->options.workers!=1&&b->options.workers!=2&&b->options.workers!=4)||
    (b->options.block!=262144&&b->options.block!=1048576&&b->options.block!=4194304)){
    fail(b,"Invalid benchmark settings.");goto finish;
  }
  if(!bench_inspect(b->target.path,b->target.name,&current)||current.device!=b->target.device||current.inode!=b->target.inode){fail(b,"Storage changed or is no longer mounted.");goto finish;}
  if(current.network&&!(b->options.tests&3)){fail(b,"NAS supports sequential tests only. Select sequential read or write.");goto finish;}
  if(!nas_uncached(&current)){fail(b,"Disable NAS write cache in Network Storage and reconnect before testing. Cached uploads would produce misleading results.");goto finish;}
  const uint64_t reserve=256000000ULL+b->options.size/10;
  if(current.free_bytes<b->options.size+reserve){fail(b,"Not enough free space; the test size plus a free-space reserve is required.");goto finish;}
  root=open(current.path,O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW);if(root<0){fail(b,"Could not open storage.");goto finish;}
  struct stat pinned;if(fstat(root,&pinned)||pinned.st_dev!=current.device||pinned.st_ino!=current.inode){fail(b,"Storage changed before the test.");goto finish;}
  unsigned char random[8];int entropy=open("/dev/urandom",O_RDONLY|O_CLOEXEC);
  if(entropy<0||read(entropy,random,sizeof(random))!=(ssize_t)sizeof(random)){if(entropy>=0)close(entropy);fail(b,"Could not create a unique test identity.");goto finish;}close(entropy);
  snprintf(name,sizeof(name),"%s%02x%02x%02x%02x%02x%02x%02x%02x",prefix,random[0],random[1],random[2],random[3],random[4],random[5],random[6],random[7]);
  if(mkdirat(root,name,0700)){name[0]=0;fail(b,"Could not create a new test folder. Nothing existing was modified.");goto finish;}
  directory=openat(root,name,O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW);if(directory<0){fail(b,"Could not pin the test folder.");goto finish;}
  int owner=openat(directory,"owner",O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC|O_NOFOLLOW,0600);
  if(owner<0||write(owner,marker,strlen(marker))!=(ssize_t)strlen(marker)){if(owner>=0)close(owner);fail(b,"Could not mark the temporary folder.");goto finish;}close(owner);
  file=openat(directory,"data.bin",O_RDWR|O_CREAT|O_EXCL|O_CLOEXEC|O_NOFOLLOW,0600);
  if(file<0){fail(b,"Could not create a new test file. Nothing existing was overwritten.");goto finish;}close(file);file=-1;
  pthread_mutex_lock(&b->mutex);
  for(unsigned i=0;i<BENCH_TESTS;i++)if(b->options.tests&(1U<<i)){
    if(current.network&&i>=2){b->snapshot.results[i].skipped=1;continue;}
    b->snapshot.total+=b->options.repeats;
  }pthread_mutex_unlock(&b->mutex);
  stage(b,"Preparing test file","Writing a new temporary file; this preparation is not a benchmark result.");
  struct measurement prepared;
  if(!run_fio(b,directory,1,1,&prepared))goto finish;
  for(unsigned pass=0;pass<b->options.repeats&&!atomic_load(&b->cancel);pass++)for(unsigned test=0;test<BENCH_TESTS&&!atomic_load(&b->cancel);test++){
    if(!(b->options.tests&(1U<<test))||(current.network&&test>=2))continue;
    if(current.network&&test==1){
      if(unlinkat(directory,"data.bin",0)){fail(b,"Could not reset the owned NAS test file.");goto finish;}
      int fresh=openat(directory,"data.bin",O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0600);
      if(fresh<0){fail(b,"Could not create the next NAS test file.");goto finish;}close(fresh);
    }
    char detail[256];snprintf(detail,sizeof(detail),"Run %u of %u / %s",pass+1,b->options.repeats,current.network?"NAS/FUSE transfer; buffering may affect results":b->options.direct?"Direct I/O":"Buffered I/O; RAM cache may affect results");
    stage(b,bench_names[test],detail);struct measurement m;if(!run_fio(b,directory,(int)test,0,&m))goto finish;
    pthread_mutex_lock(&b->mutex);struct measurement *result=&b->snapshot.results[test];
    result->mb_s+=m.mb_s;result->iops+=m.iops;result->latency_ms+=m.latency_ms;result->samples++;
    b->snapshot.completed++;b->snapshot.progress=100.0*b->snapshot.completed/b->snapshot.total;pthread_mutex_unlock(&b->mutex);
  }
finish:
  stage(b,"Cleaning up","Removing only this benchmark's own temporary files.");
  if(file>=0)close(file);if(directory>=0)close(directory);
  if(root>=0&&name[0]&&!remove_owned(root,name)){
    pthread_mutex_lock(&b->mutex);snprintf(b->snapshot.cleanup,sizeof(b->snapshot.cleanup),"Temporary test folder remains: %s",name);pthread_mutex_unlock(&b->mutex);
  }
  if(root>=0)close(root);
  if(lock>=0)close(lock);
  struct snapshot snapshot;bench_snapshot(b,&snapshot);
  stage(b,atomic_load(&b->cancel)?"Benchmark cancelled":snapshot.error[0]?"Benchmark failed":"Benchmark complete",snapshot.error[0]?snapshot.error:snapshot.cleanup[0]?snapshot.cleanup:"Temporary test files removed.");
  atomic_store(&b->done,1);return NULL;
}
