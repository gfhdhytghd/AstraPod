/* Bounded text logs, including their one backup. GPL v2 or later.
 * Copy-before-truncate deliberately keeps the current inode/name alive. A
 * failed copy/fsync never truncates it. The backup is durable before the
 * current is cleared. The small copy buffer and paths are shared under lock;
 * neither the UI nor the storage thread needs a large rotation stack. */
#include "config.h"
#include "logfile.h"
#include "file.h"
#include "dir.h"
#include "kernel.h"
#include "thread.h"
#include "storage.h"
#include "vuprintf.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>

#define LOGFILE_HANDLES 16
static struct {
    int fd;
    char backup[MAX_PATH];
    const char *header;
} handles[LOGFILE_HANDLES];
static struct mutex file_lock;
static unsigned char copy_buffer[512];
static char old_path[MAX_PATH];
static char cleaned[24][MAX_PATH];
static unsigned int cleaned_count;
static unsigned char numbered[10000 / 8];
static bool ready;
static unsigned int ui_thread;
enum { LOG_OPEN=1, LOG_WRITE, LOG_CLOSE, LOG_SIZE, LOG_PRINT };
struct job {
    int op, fd;
    const char *path, *header, *format;
    const void *data;
    size_t size;
    va_list *args;
    ssize_t result;
};

static bool write_all(int fd, const void *data, size_t size)
{
    const char *p = data;
    while (size) {
        ssize_t n = write(fd, p, size);
        if (n <= 0) return false;
        p += n; size -= n;
    }
    return true;
}

/* Called only as part of an actual append. No timer or disk-wake callback. */
static bool rotate(int slot)
{
    int fd = handles[slot].fd;
    off_t size = ffilesize(fd);
    if (size < 0) return false;
    int backup = open(handles[slot].backup, O_WRONLY|O_CREAT|O_TRUNC, 0666);
    if (backup < 0) return false;
    const char *header=handles[slot].header;
    size_t h=header?strlen(header):0;
    off_t start = size > (off_t)LOGFILE_PART_BYTES ? size - (LOGFILE_PART_BYTES-h) : 0;
    bool ok = lseek(fd, start, SEEK_SET) >= 0;
    if (ok && start && h) ok=write_all(backup,header,h);
    unsigned int chunks = 0;
    while (ok && start < size) {
        size_t want = MIN((off_t)sizeof copy_buffer, size - start);
        ssize_t n = read(fd, copy_buffer, want);
        if (n <= 0 || !write_all(backup, copy_buffer, n)) {ok = false; break;}
        start += n;
        if (++chunks % 16 == 0) yield();
    }
    if (ok) ok = fsync(backup) == 0;
    close(backup);
    /* Also safe on a copy/fsync failure: keep the entire current log. */
    if (ok) ok = ftruncate(fd, 0) == 0;
    lseek(fd, 0, SEEK_END);
    return ok;
}

static void backup_path(const char *path, int n, char *out, size_t size)
{
    const char *dot = strrchr(path, '.');
    const char *slash = strrchr(path, '/');
    if (!dot || (slash && dot < slash)) dot = path + strlen(path);
    snprintf(out, size, "%.*s.%d%s", (int)(dot-path), path, n, dot);
}

/* Playback's reader already walks playback_0001.log before playback.log.
 * Adopt the newest old numbered file, discard older history, and reuse that
 * single reader-compatible backup thereafter. Only run at its next write. */
static bool playback_backups(const char *path, char *backup, size_t size)
{
    const char *slash = strrchr(path, '/');
    if (!slash || strcmp(slash+1, "playback.log")) return true;
    snprintf(old_path, sizeof old_path, "%.*s", (int)(slash-path), path);
    DIR *dir = opendir(old_path);
    struct dirent *entry;
    int newest = 1;
    unsigned int visited=0;
    memset(numbered,0,sizeof numbered);
    if (!dir) return false;
    while ((entry = readdir(dir))) {
        int n=0,end=0;
        if (sscanf(entry->d_name,"playback_%4d.log%n",&n,&end)==1 &&
            end==(int)strlen(entry->d_name) && n>0 && n<10000) {
            numbered[n/8] |= 1u << (n%8);
            if (n>newest) newest=n;
        }
        if (++visited%16==0) yield();
    }
    closedir(dir);
    snprintf(backup,size,"%.*s/playback_0001.log",(int)(slash-path),path);
    /* Scan once, then remove oldest first. No deletion under the FAT
     * directory iterator, and no repeated scans of a long history. */
    for (int n=1;n<newest;n++) {
        if (numbered[n/8] & (1u << (n%8))) {
            snprintf(old_path,sizeof old_path,"%.*s/playback_%04d.log",(int)(slash-path),path,n);
            if (remove(old_path)<0) return false;
        }
        if (n%16==0) yield();
    }
    if (newest>1) {
        snprintf(old_path,sizeof old_path,"%.*s/playback_%04d.log",(int)(slash-path),path,newest);
        if (rename(old_path,backup)<0) return false;
    }
    return true;
}

static int open_log(const char *path, const char *header)
{
    if (strlen(path) + 12 >= MAX_PATH ||
        (header && strlen(header)>=LOGFILE_PART_BYTES)) return -1;
    int slot;
    for (slot = 0; slot < LOGFILE_HANDLES; slot++) if (handles[slot].fd < 0) break;
    if (slot == LOGFILE_HANDLES) return -1;
    int fd = open(path, O_RDWR|O_CREAT|O_APPEND, 0666);
    if (fd < 0) return -1;
    handles[slot].fd = fd;
    backup_path(path,1,handles[slot].backup,sizeof handles[slot].backup);
    bool first=true;
    for (unsigned int i=0;i<cleaned_count;i++)
        if (!strcmp(cleaned[i],path)) {first=false;break;}
    const char *slash=strrchr(path,'/');
    if (slash && !strcmp(slash+1,"playback.log"))
        snprintf(handles[slot].backup,sizeof handles[slot].backup,
                 "%.*s/playback_0001.log",(int)(slash-path),path);
    if (first) {
        /* The old diagnostics kept .1 through .4; remove oldest first. */
        for (int n=4;n>=2;n--) {
            backup_path(path,n,old_path,sizeof old_path);
            if (remove(old_path)<0 && errno!=ENOENT) goto fail;
        }
        if (!playback_backups(path,handles[slot].backup,sizeof handles[slot].backup)) goto fail;
        if (cleaned_count<ARRAYLEN(cleaned))
            snprintf(cleaned[cleaned_count++],MAX_PATH,"%s",path);
    }
    int old = open(handles[slot].backup,O_RDONLY);
    if (old >= 0) {
        off_t bytes=ffilesize(old);close(old);
        if (bytes > (off_t)LOGFILE_PART_BYTES && remove(handles[slot].backup)<0) goto fail;
    }
    handles[slot].header = header;
    if (ffilesize(fd) > (off_t)LOGFILE_PART_BYTES && !rotate(slot)) {
        close(fd);handles[slot].fd=-1;return -1;
    }
    if (header && ffilesize(fd)==0 && !write_all(fd,header,strlen(header))) goto fail;
    return fd;
fail:
    close(fd);handles[slot].fd=-1;return -1;
}

static ssize_t append(int slot, const void *data, size_t size)
{
    int fd=handles[slot].fd;const char *p=data;size_t done=0;
    const char *header=handles[slot].header;
    size_t h=header?strlen(header):0;
    if (h >= LOGFILE_PART_BYTES) return -1;
    while (done < size) {
        size_t n=MIN(size-done,LOGFILE_PART_BYTES-h);
        off_t bytes=ffilesize(fd);
        if (bytes < 0) return -1;
        if (bytes+(off_t)n > (off_t)LOGFILE_PART_BYTES) {
            if (!rotate(slot)) return -1;
            bytes=0;
        }
        if (!bytes && h && !write_all(fd,header,h)) return -1;
        if (!write_all(fd,p+done,n)) return -1;
        done+=n;
    }
    return done;
}

static int print_log(int slot,const char *format,va_list *args);

static void perform(struct job *j)
{
    mutex_lock(&file_lock);
    j->result=-1;
    if (j->op == LOG_OPEN) j->result=open_log(j->path,j->header);
    else for (int i=0;i<LOGFILE_HANDLES;i++) if (handles[i].fd == j->fd) {
        if (j->op==LOG_WRITE) j->result=append(i,j->data,j->size);
        else if (j->op==LOG_PRINT) j->result=print_log(i,j->format,j->args);
        else if (j->op==LOG_SIZE) j->result=ffilesize(j->fd);
        else if (j->op==LOG_CLOSE) {j->result=close(j->fd);handles[i].fd=-1;}
        break;
    }
    mutex_unlock(&file_lock);
}
void logfile_storage_request(void *request, bool available)
{
    struct job *j=request;
    /* A session opened before USB ownership changed must still release its
     * descriptor. Refusing close would pin the disk and exhaust log handles. */
    if (available || j->op==LOG_CLOSE) perform(j);
    else j->result=-1;
}
static ssize_t dispatch(struct job *j)
{
    if (!ready) return -1;
    if (thread_self()==ui_thread) {
        if (!storage_logfile_call(j)) return -1; /* never rotate on UI */
    } else perform(j);
    return j->result;
}
void logfile_init(void)
{
    if (ready) return;
    mutex_init(&file_lock);
    for (int i=0;i<LOGFILE_HANDLES;i++) handles[i].fd=-1;
    ui_thread=thread_self();
    ready=true;
}
int logfile_open_header(const char *path,const char *header)
{struct job j={.op=LOG_OPEN,.path=path,.header=header};return dispatch(&j);}
int logfile_open(const char *path) {return logfile_open_header(path,NULL);}
ssize_t logfile_write(int fd,const void *data,size_t size)
{struct job j={.op=LOG_WRITE,.fd=fd,.data=data,.size=size};return dispatch(&j);}
off_t logfile_size(int fd) {struct job j={.op=LOG_SIZE,.fd=fd};return dispatch(&j);}
int logfile_close(int fd) {struct job j={.op=LOG_CLOSE,.fd=fd};return dispatch(&j);}
struct print_state {int slot;unsigned int used;bool error;char line[256];};
static int print_char(void *user, int c)
{
    struct print_state *state=user;
    state->line[state->used++]=c;
    if (state->used==sizeof state->line) {
        if (append(state->slot,state->line,state->used)!=(ssize_t)state->used) {
            state->error=true;return 0;
        }
        state->used=0;
    }
    return 1;
}
static int print_log(int slot,const char *format,va_list *args)
{
    struct print_state state={.slot=slot};
    int n=vuprintf(print_char,&state,format,*args);
    if (state.used && !state.error &&
        append(slot,state.line,state.used)!=(ssize_t)state.used) state.error=true;
    return state.error?-1:n;
}
int logfile_printf(int fd,const char *format,...)
{
    va_list ap;va_start(ap,format);
    struct job j={.op=LOG_PRINT,.fd=fd,.format=format,.args=&ap};
    int n=dispatch(&j);va_end(ap);
    return n;
}
