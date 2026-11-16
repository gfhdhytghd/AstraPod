"""FU41: real device log writer on a POSIX disk, pthread/queue adapters."""
from pathlib import Path
import os, subprocess, sys, tempfile
R=Path(__file__).resolve().parents[2]
O=Path(sys.argv[1]).resolve();O.mkdir(parents=True,exist_ok=True)
(O/'config.h').write_text('''#pragma once
#define MAX_PATH 260
#define MIN(a,b) ((a)<(b)?(a):(b))
#define ARRAYLEN(a) (sizeof(a)/sizeof((a)[0]))
#define DEFAULT_STACK_SIZE 2048
#define IF_PRIO(...)
#define IF_COP(...)
''')
(O/'vuprintf.h').write_text('#include <stdarg.h>\nint vuprintf(int(*)(void*,int),void*,const char*,va_list);\n')
(O/'gcc_extensions.h').write_text('#define ATTRIBUTE_PRINTF(a,b) __attribute__((format(printf,a,b)))\n')
(O/'file.h').write_text('''#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
int test_open(const char*,int,...);
ssize_t test_write(int,const void*,size_t);
int test_sync(int);
int test_truncate(int,off_t);
off_t test_size(int);
#define open test_open
#define write test_write
#define fsync test_sync
#define ftruncate test_truncate
#define ffilesize test_size
''')
(O/'dir.h').write_text('#include <dirent.h>\n')
(O/'kernel.h').write_text('''#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <pthread.h>
struct mutex {pthread_mutex_t value;};
struct queue_event {long id;intptr_t data;};
struct event_queue {pthread_mutex_t lock;pthread_cond_t wake;bool waiting,done;struct queue_event event;};
struct queue_sender_list {int n;};
void mutex_init(struct mutex*);void mutex_lock(struct mutex*);void mutex_unlock(struct mutex*);
void queue_init(struct event_queue*,bool);void queue_wait(struct event_queue*,struct queue_event*);
void queue_reply(struct event_queue*,intptr_t);intptr_t queue_send(struct event_queue*,long,intptr_t);
void queue_enable_queue_send(struct event_queue*,struct queue_sender_list*,unsigned);
void yield(void);
''')
(O/'storage.h').write_text('#include <stdbool.h>\nbool storage_logfile_call(void*);\n')
(O/'thread.h').write_text('''#include <stddef.h>
unsigned thread_self(void);
unsigned create_thread(void(*)(void),void*,size_t,unsigned,const char*);
''')
source=r'''
#define _GNU_SOURCE
#include "config.h"
#include "kernel.h"
#include "logfile.h"
#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
int vuprintf(int(*push)(void*,int),void*user,const char*fmt,va_list ap){
 char data[4096];int n=vsnprintf(data,sizeof data,fmt,ap);assert(n>=0&&n<(int)sizeof data);
 for(int i=0;i<n;i++)if(!push(user,data[i]))return i;return n;
}
static pthread_t main_thread;
static int backup_fd=-1,cut_phase,fail_sync,fail_copy,queue_calls,rotations;
static const char *root;
void mutex_init(struct mutex*m){assert(!pthread_mutex_init(&m->value,NULL));}
void mutex_lock(struct mutex*m){assert(!pthread_mutex_lock(&m->value));}
void mutex_unlock(struct mutex*m){assert(!pthread_mutex_unlock(&m->value));}
void queue_init(struct event_queue*q,bool b){(void)b;pthread_mutex_init(&q->lock,NULL);pthread_cond_init(&q->wake,NULL);q->waiting=q->done=false;}
void queue_wait(struct event_queue*q,struct queue_event*e){pthread_mutex_lock(&q->lock);while(!q->waiting)pthread_cond_wait(&q->wake,&q->lock);*e=q->event;pthread_mutex_unlock(&q->lock);}
void queue_reply(struct event_queue*q,intptr_t n){(void)n;pthread_mutex_lock(&q->lock);q->waiting=false;q->done=true;pthread_cond_broadcast(&q->wake);pthread_mutex_unlock(&q->lock);}
intptr_t queue_send(struct event_queue*q,long id,intptr_t data){
 assert(pthread_equal(pthread_self(),main_thread));queue_calls++;
 pthread_mutex_lock(&q->lock);assert(!q->waiting);q->done=false;q->event=(struct queue_event){id,data};q->waiting=true;pthread_cond_broadcast(&q->wake);
 while(!q->done)pthread_cond_wait(&q->wake,&q->lock);pthread_mutex_unlock(&q->lock);return 0;
}
void queue_enable_queue_send(struct event_queue*q,struct queue_sender_list*s,unsigned id){(void)q;(void)s;assert(id);}
void yield(void){sched_yield();}
unsigned thread_self(void){return pthread_equal(pthread_self(),main_thread)?1:2;}
static struct event_queue disk_jobs;
static bool disk_started,disk_available=true;
static void*start_worker(void*p){
 (void)p;struct queue_event event;
 while(true){queue_wait(&disk_jobs,&event);logfile_storage_request((void*)event.data,disk_available);queue_reply(&disk_jobs,0);}
 return NULL;
}
bool storage_logfile_call(void*request){
 if(!disk_started){queue_init(&disk_jobs,false);pthread_t t;assert(!pthread_create(&t,NULL,start_worker,NULL));pthread_detach(t);disk_started=true;}
 queue_send(&disk_jobs,1,(intptr_t)request);return true;
}
int test_open(const char*p,int flags,...){int fd=open(p,flags,0666);if(flags&O_TRUNC)backup_fd=fd;return fd;}
off_t test_size(int fd){struct stat s;return fstat(fd,&s)?-1:s.st_size;}
ssize_t test_write(int fd,const void*p,size_t n){
 if(fd==backup_fd){assert(!pthread_equal(pthread_self(),main_thread));if(fail_copy){fail_copy=0;errno=ENOSPC;return -1;}}
 ssize_t result=write(fd,p,n);
 if(fd==backup_fd && cut_phase==1)_exit(77);
 return result;
}
int test_sync(int fd){
 if(fd==backup_fd){assert(!pthread_equal(pthread_self(),main_thread));if(fail_sync){fail_sync=0;errno=EIO;return -1;}}
 int n=fsync(fd);if(cut_phase==2)_exit(77);return n;
}
int test_truncate(int fd,off_t n){
 assert(!pthread_equal(pthread_self(),main_thread));rotations++;
 if(cut_phase==3)_exit(77);
 int result=ftruncate(fd,n);if(cut_phase==4)_exit(77);return result;
}
static void path(char*p,const char*name){snprintf(p,260,"%s/%s",root,name);}
static void backup_path(const char*p,char*out){
 const char*dot=strrchr(p,'.');const char*slash=strrchr(p,'/');
 if(!dot || (slash&&dot<slash))dot=p+strlen(p);
 snprintf(out,260,"%.*s.1%s",(int)(dot-p),p,dot);
 if(slash&&!strcmp(slash+1,"playback.log"))snprintf(out,260,"%.*s/playback_0001.log",(int)(slash-p),p);
}
static off_t size(const char*p){struct stat s;return stat(p,&s)?0:s.st_size;}
static void bounded(const char*p){char backup[260];backup_path(p,backup);assert(size(p)<=LOGFILE_PART_BYTES&&size(backup)<=LOGFILE_PART_BYTES);assert(size(p)+size(backup)<=LOGFILE_TOTAL_BYTES);assert(!access(p,F_OK));}
static bool contains(const char*p,const char*needle){
 int fd=open(p,O_RDONLY);if(fd<0)return false;
 char *data=malloc(LOGFILE_TOTAL_BYTES+1);ssize_t n=read(fd,data,LOGFILE_TOTAL_BYTES);close(fd);assert(n>=0);data[n]=0;bool yes=strstr(data,needle)!=NULL;free(data);return yes;
}
static void seed(const char*p,size_t n){
 int fd=open(p,O_WRONLY|O_CREAT|O_TRUNC,0666);assert(fd>=0);char block[4096];memset(block,'o',sizeof block);
 for(size_t i=0;i<n;i+=sizeof block){size_t m=MIN(n-i,sizeof block);assert(write(fd,block,m)==(ssize_t)m);}
 if(n>=7){lseek(fd,n-7,SEEK_SET);assert(write(fd,"OLDEND\n",7)==7);}close(fd);
}
static void fill(const char*name){
 char p[260],backup[260];path(p,name);backup_path(p,backup);int fd=logfile_open(p);assert(fd>=0);
 char *block=malloc(65536);memset(block,'x',65536);block[65535]='\n';
 for(unsigned n=0;n<3*LOGFILE_PART_BYTES/65536+1;n++){assert(logfile_write(fd,block,65536)==65536);bounded(p);}
 assert(logfile_printf(fd,"LATEST %s\n",name)>0);assert(!logfile_close(fd));free(block);
 assert(contains(p,"LATEST "));bounded(p);printf("FU41 %s past cap, newest record retained PASS\n",name);
}
static pthread_barrier_t barrier;
static void*concurrent(void*arg){
 char p[260],line[256];path(p,"concurrent.log");int fd=logfile_open(p);assert(fd>=0);long id=(long)arg;
 memset(line,'x',sizeof line);line[255]='\n';
 for(int n=0;n<9000;n++){assert(logfile_write(fd,line,sizeof line)==sizeof line);}
 pthread_barrier_wait(&barrier);assert(logfile_printf(fd,"FINAL %ld\n",id)>0);assert(!logfile_close(fd));return NULL;
}
int main(int argc,char**argv){
 assert(argc>=2);root=argv[1];main_thread=pthread_self();logfile_init();
 char p[260],backup[260];path(p,"fault.log");backup_path(p,backup);
 if(argc>2){
  if(!strcmp(argv[2],"cut")){seed(p,LOGFILE_PART_BYTES);cut_phase=atoi(argv[3]);}
  int fd=logfile_open(p);assert(fd>=0);assert(logfile_printf(fd,"RECOVERED\n")>0);assert(!logfile_close(fd));bounded(p);return 0;
 }
 disk_available=false;path(p,"usb-blocked.log");assert(logfile_open(p)<0&&access(p,F_OK)<0);disk_available=true;
 path(p,"usb-close.log");int usbfd=logfile_open(p);assert(usbfd>=0);disk_available=false;
 assert(logfile_write(usbfd,"NO WRITE",8)<0);assert(!logfile_close(usbfd));assert(fcntl(usbfd,F_GETFD)<0&&errno==EBADF);disk_available=true;
 const char*names[]={"tagcache.log","art.log","usb.log","battery.csv","playback_errors.log","mikey.log","metadata.log","theme.log","tasks.log","upgrade.log","buffer_damage.log","remote-log.txt","playback.log",".scrobbler.log","rockbox_log.txt","logf.txt"};
 for(unsigned i=0;i<ARRAYLEN(names);i++)fill(names[i]);
 path(p,"long.log");int longfd=logfile_open(p);assert(longfd>=0);
 char longline[1025];memset(longline,'L',sizeof longline-1);longline[sizeof longline-1]=0;
 assert(logfile_printf(longfd,"%s END\n",longline)==1029);assert(!logfile_close(longfd));assert(size(p)==1029);
 /* Preserve the Audioscrobbler header after a rotation in the same session. */
 path(p,"header.log");int fd=logfile_open_header(p,"#HEADER\n");assert(fd>=0&&size(p)==8);char*big=malloc(LOGFILE_PART_BYTES);memset(big,'x',LOGFILE_PART_BYTES);
 assert(logfile_write(fd,big,LOGFILE_PART_BYTES)==LOGFILE_PART_BYTES);assert(logfile_printf(fd,"HEADERLATEST\n")>0);assert(!logfile_close(fd));free(big);bounded(p);
 char prefix[9]={0};int raw=open(p,O_RDONLY);assert(read(raw,prefix,8)==8&&!strcmp(prefix,"#HEADER\n"));close(raw);
 path(p,"old-header.log");seed(p,LOGFILE_PART_BYTES+100);fd=logfile_open_header(p,"#HEADER\n");assert(fd>=0);assert(logfile_printf(fd,"MIGRATED\n")>0);assert(!logfile_close(fd));bounded(p);backup_path(p,backup);
 raw=open(backup,O_RDONLY);memset(prefix,0,sizeof prefix);assert(read(raw,prefix,8)==8&&!strcmp(prefix,"#HEADER\n"));close(raw);assert(contains(backup,"OLDEND\n"));
 for(int failure=0;failure<2;failure++){
  path(p,"fault.log");seed(p,LOGFILE_PART_BYTES);fd=logfile_open(p);assert(fd>=0);
  if(failure)fail_sync=1;else fail_copy=1;
  assert(logfile_printf(fd,"NEWEST\n")==-1);assert(contains(p,"OLDEND\n"));bounded(p);
  assert(logfile_printf(fd,"NEWEST\n")>0);assert(!logfile_close(fd));assert(contains(p,"NEWEST\n"));
 }
 /* Upgrade old multi-backup diagnostics and numbered playback history. */
 path(p,"legacy.log");seed(p,LOGFILE_PART_BYTES+123);
 for(int i=2;i<=4;i++){snprintf(backup,260,"%s/legacy.%d.log",root,i);seed(backup,100);}
 fd=logfile_open(p);assert(fd>=0);assert(logfile_printf(fd,"UPGRADED\n")>0);assert(!logfile_close(fd));bounded(p);
 for(int i=2;i<=4;i++){snprintf(backup,260,"%s/legacy.%d.log",root,i);assert(access(backup,F_OK)<0);}
 /* A different directory avoids the already migrated playback fixture. */
 char subdir[260];snprintf(subdir,260,"%s/legacy",root);assert(!mkdir(subdir,0700));
 for(int i=1;i<=3;i++){snprintf(backup,260,"%s/legacy/playback_%04d.log",root,i);seed(backup,100+i);}
 snprintf(p,260,"%s/legacy/playback.log",root);fd=logfile_open(p);assert(fd>=0);assert(logfile_printf(fd,"NOW\n")>0);assert(!logfile_close(fd));bounded(p);
 snprintf(backup,260,"%s/legacy/playback_0001.log",root);assert(size(backup)==103);
 for(int i=2;i<=3;i++){snprintf(backup,260,"%s/legacy/playback_%04d.log",root,i);assert(access(backup,F_OK)<0);}
 int sends_before=queue_calls;pthread_barrier_init(&barrier,NULL,4);pthread_t threads[4];for(long i=0;i<4;i++)assert(!pthread_create(&threads[i],NULL,concurrent,(void*)i));
 for(int i=0;i<4;i++)pthread_join(threads[i],NULL);assert(queue_calls==sends_before);path(p,"concurrent.log");backup_path(p,backup);bounded(p);
 for(int i=0;i<4;i++){char marker[32];snprintf(marker,sizeof marker,"FINAL %d\n",i);assert(contains(p,marker)||contains(backup,marker));}
 assert(queue_calls>0&&rotations>0);printf("FU41 concurrent sessions, copy/sync failure, legacy upgrade, durable header, UI relay PASS (queue=%d rotations=%d)\n",queue_calls,rotations);
}
'''
(O/'test.c').write_text(source)
exe=O/'test'
subprocess.run(['cc','-std=gnu11','-Wall','-Wextra','-Werror','-Wno-misleading-indentation','-g','-fsanitize=address,undefined','-fno-pie','-no-pie','-pthread','-I'+str(O),'-I'+str(R/'firmware/export'),str(O/'test.c'),str(R/'firmware/logfile.c'),'-o',str(exe)],check=True)
run=Path(tempfile.mkdtemp(prefix='run-',dir=O))
normal=run/'normal';normal.mkdir()
subprocess.run([str(exe),str(normal)],check=True,timeout=120)
for phase in range(1,5):
    d=run/f'cut-{phase}';d.mkdir(exist_ok=True)
    result=subprocess.run([str(exe),str(d),'cut',str(phase)],timeout=30)
    assert result.returncode==77,(phase,result.returncode)
    current=d/'fault.log';backup=d/'fault.1.log'
    assert current.exists() and sum(p.stat().st_size for p in (current,backup))<=5*1024*1024
    assert b'OLDEND\n' in current.read_bytes()+backup.read_bytes()
    subprocess.run([str(exe),str(d),'resume'],check=True,timeout=30)
    assert current.read_bytes().endswith(b'RECOVERED\n')
    print(f'FU41 power loss phase={phase}: current exists, latest old data retained, restart bounded PASS')
# Source wiring, not just the shared algorithm: every active disk writer uses it.
for name in ['apps-ipod/system/debug_log.c','apps-ipod/system/library_files.c','apps-ipod/audio/playback.c','apps-ipod/audio/buffering.c','apps-ipod/screens/system/remote_input.c','apps-ipod/screens/system/log_viewer.c','firmware/logf.c']:
    assert 'logfile_' in (R/name).read_text(),name
for name in ['usb_log_file.c','battery_log.c']:
    s=(R/'apps-ipod/system'/name).read_text();assert 'logfile_printf(' in s and 'fdprintf(' not in s
assert 'metadata_log_write = metadata_line;' in (R/'apps-ipod/system/debug_log.c').read_text()
assert 'metadata_log_write(trackname, res_str);' in (R/'lib/rbcodec/metadata/metadata.c').read_text()
assert '#define DEBUG_LOG_KEEP 1' in (R/'apps-ipod/system/debug_log.h').read_text()
assert 'create_numbered_filename' not in (R/'apps-ipod/audio/playback.c').read_text()
print('FU41 production writer inventory wiring PASS')

assert 'create_thread(' not in (R/'firmware/logfile.c').read_text()
assert 'logfile_storage_request((void *)ev.data, !usb_mode);' in (R/'firmware/storage.c').read_text()
print('FU41 existing storage relay, direct storage/background path, USB ownership gate PASS')
