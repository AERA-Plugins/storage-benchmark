/* SPDX-License-Identifier: Apache-2.0 */
#define _GNU_SOURCE
#include "settings.h"
#define JSMN_STATIC
#include "../vendor/jsmn/jsmn.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/xattr.h>
#include <unistd.h>

static const char *directory(void){
#ifdef BENCH_HOST_TEST
  const char *fixture=getenv("BENCH_SETTINGS_DIR");if(fixture)return fixture;
#endif
  return "/data/media/0/AERA/StorageBenchmark";
}
static int valid(const struct options *o){
  return o->size>=128000000&&o->size<=4000000000ULL&&!(o->size%4096)&&
    (o->seconds==5||o->seconds==10||o->seconds==20||o->seconds==30)&&
    o->repeats>=1&&o->repeats<=3&&(o->workers==1||o->workers==2||o->workers==4)&&
    (o->block==262144||o->block==1048576||o->block==4194304)&&!(o->tests&~0x3fU)&&
    (o->direct==0||o->direct==1);
}
void settings_load(struct options *options,char target[PATH_MAX]){
  int dir=open(directory(),O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW);if(dir<0)return;
  int fd=openat(dir,"preferences.json",O_RDONLY|O_CLOEXEC|O_NOFOLLOW);close(dir);if(fd<0)return;
  char input[16384];ssize_t size=read(fd,input,sizeof(input)-1);close(fd);if(size<=0)return;input[size]=0;
  jsmntok_t tokens[64];jsmn_parser parser;jsmn_init(&parser);
  int count=jsmn_parse(&parser,input,(size_t)size,tokens,64);
  if(count<1||tokens[0].type!=JSMN_OBJECT)return;
  struct options loaded=*options;char remembered[PATH_MAX]={0};unsigned version=0;
  for(int i=1;i+1<count;i+=2){
    jsmntok_t key=tokens[i],value=tokens[i+1];char name[32];
    if(key.type!=JSMN_STRING||key.end-key.start>=(int)sizeof(name))return;
    snprintf(name,sizeof(name),"%.*s",key.end-key.start,input+key.start);
    if(!strcmp(name,"target")){
      if(value.type!=JSMN_STRING||value.end-value.start>=PATH_MAX)return;
      size_t out=0;for(int n=value.start;n<value.end;n++){
        unsigned char c=(unsigned char)input[n];
        if(c=='\\'){
          if(++n>=value.end)return;c=(unsigned char)input[n];
          if(c!='\\'&&c!='"'&&c!='/')return;
        }
        if(c<32)return;remembered[out++]=(char)c;
      }remembered[out]=0;continue;
    }
    if(value.type!=JSMN_PRIMITIVE)return;
    char number[32];if(value.end-value.start>=(int)sizeof(number))return;
    snprintf(number,sizeof(number),"%.*s",value.end-value.start,input+value.start);
    char *end;errno=0;unsigned long long n=strtoull(number,&end,10);if(errno||*end||number[0]=='-')return;
    if(strcmp(name,"size")&&n>0xffffffffU)return;
    if(!strcmp(name,"version"))version=(unsigned)n;
    else if(!strcmp(name,"size"))loaded.size=n;
    else if(!strcmp(name,"block"))loaded.block=(unsigned)n;
    else if(!strcmp(name,"seconds"))loaded.seconds=(unsigned)n;
    else if(!strcmp(name,"repeats"))loaded.repeats=(unsigned)n;
    else if(!strcmp(name,"workers"))loaded.workers=(unsigned)n;
    else if(!strcmp(name,"tests"))loaded.tests=(unsigned)n;
    else if(!strcmp(name,"direct"))loaded.direct=(int)n;
  }
  if(version!=1||!valid(&loaded)||(remembered[0]&&remembered[0]!='/'))return;
  *options=loaded;snprintf(target,PATH_MAX,"%s",remembered);
}
int settings_save(const struct options *options,const char *target){
  if(!valid(options)||strlen(target)>=PATH_MAX)return 0;
  if(mkdir(directory(),0775)&&errno!=EEXIST)return 0;
  int dir=open(directory(),O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW);if(dir<0)return 0;
  struct stat info;if(fstat(dir,&info)){close(dir);return 0;}
  if(info.st_uid==0){fchown(dir,1023,1023);fchmod(dir,0775);}
  char temporary[80];snprintf(temporary,sizeof(temporary),".preferences-%ld.tmp",(long)getpid());
  int fd=openat(dir,temporary,O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC|O_NOFOLLOW,0664);
  if(fd<0){close(dir);return 0;}
  fchown(fd,info.st_uid==0?1023:info.st_uid,info.st_uid==0?1023:info.st_gid);fchmod(fd,0664);
  char context[256];ssize_t length=fgetxattr(dir,"security.selinux",context,sizeof(context));
  if(length>0)fsetxattr(fd,"security.selinux",context,(size_t)length,0);
  FILE *output=fdopen(fd,"w");int okay=output!=NULL;
  if(output){
    if(fprintf(output,"{\n  \"version\": 1,\n  \"size\": %llu,\n  \"block\": %u,\n  \"seconds\": %u,\n  \"repeats\": %u,\n  \"workers\": %u,\n  \"tests\": %u,\n  \"direct\": %d,\n  \"target\": \"",(unsigned long long)options->size,options->block,options->seconds,options->repeats,options->workers,options->tests,options->direct)<0)okay=0;
    for(const unsigned char *p=(const unsigned char *)target;*p;p++){
      if(*p<32){okay=0;break;}
      if((*p=='"'||*p=='\\')&&fputc('\\',output)==EOF)okay=0;
      if(fputc(*p,output)==EOF)okay=0;
    }
    if(fputs("\"\n}\n",output)==EOF||fflush(output)||fsync(fd))okay=0;
    if(fclose(output))okay=0;
  }else close(fd);
  if(okay&&renameat(dir,temporary,dir,"preferences.json"))okay=0;
  if(!okay)unlinkat(dir,temporary,0);
  close(dir);return okay;
}
