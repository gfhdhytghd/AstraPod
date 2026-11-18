"""FU75 production USB flow, handoff state and PCM boundary on a synthetic dock."""
from pathlib import Path
import re
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
OUT = Path(sys.argv[1]).resolve()
OUT.mkdir(parents=True, exist_ok=True)
HANDOFF = ROOT / 'apps-ipod/screens/playback/handoff.c'
assert HANDOFF.is_file(), 'FU75 handoff is missing'


def function(path, name):
    text = (ROOT / path).read_text()
    match = re.search(r'^\s*[A-Za-z_][\w *]+\b' + name + r'\([^;{}]*\)\s*\{',
                      text, re.M)
    assert match, name
    end, depth = match.end(), 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[match.start():end]


ports = r'''
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <assert.h>
#include <ctype.h>
#define HAVE_USB_PODLINK
#define HZ 100
#define AUDIO_STATUS_PLAY 1
#define AUDIO_STATUS_PAUSE 2
#define USB_INSERTED 2
#define PODLINK_TEXT 2
#define ACTIVITY_WPS 8
#define SYS_USB_CONFIRM 9
#define BUTTON_REDRAW 10
#define SYS_EVENT 0x4000
#define BUTTON_MULTIMEDIA 0x8000
#define IS_SYSEVENT(a) ((a) & SYS_EVENT)
#define BUTTON_MULTIMEDIA_PLAYPAUSE (BUTTON_MULTIMEDIA + 1)
#define ARRAYLEN(v) (sizeof(v)/sizeof((v)[0]))
#define TIME_AFTER(a,b) ((long)((b)-(a)) < 0)
#define TIME_BEFORE(a,b) TIME_AFTER(b,a)
#define CONTEXT_STD 1
#define VP_FLAG_ALIGN_CENTER 2
#define str(n) (n)
#define LANG_HANDOFF_TITLE "Hand off?"
#define LANG_HANDOFF_BODY "%s: %s"
#define LANG_HANDOFF_ACCEPT "Hand off"
#define LANG_HANDOFF_LEAVE "Stay"
enum { LANG_HANDOFF_FAILED, LANG_HANDOFF_NOT_FOUND,
       LANG_HANDOFF_NO_RESPONSE, LANG_HANDOFF_HOST_FAILED };
enum { ACTION_STD_OK, ACTION_STD_CANCEL, ACTION_STD_MENU, ACTION_STD_PREV, ACTION_STD_NEXT,
       ACTION_WPS_PLAY, DIALOG_CONTINUE, DIALOG_ABORT, DIALOG_CANCEL, DIALOG_ACCEPT };
static long current_tick = 100;
static bool cable, active, online, config_wait;
static int status, activity, stops, usb_replies, disk_choices, redraws, splashes;
static int dialog_depth, prompts, answer, cancel_on_question, frame;
static unsigned long sample, fade, captured;
static bool pcm_hold, pcm_end;
static char log_line[320];
struct mutex { int used; };
struct podlink_message { unsigned int type, length; const unsigned char *data; };
struct mp3entry { unsigned long elapsed,length; const char *title,*artist,*album; char path[64]; };
static struct mp3entry local = { .elapsed=17000, .length=180000, .title="Song",
                                 .artist="Artist", .album="Album", .path="/Song.mp3" };
static struct mp3entry *audio_current_track(void) {return &local;}
static int audio_status(void) {return status;}
static int get_current_activity(void) {return activity;}
static bool cider_active(void) {return active;}
static bool cider_available(void) {return online;}
bool handoff_hold_source(void);
static void cider_remote_source(void) {if (!handoff_hold_source()) active=online;}
static void audio_stop(void) {status=0; stops++; pcm_end=false;}
static void pause_action(bool x) {(void)x;status |= AUDIO_STATUS_PAUSE;}
static void unpause_action(bool x) {(void)x;status &= ~AUDIO_STATUS_PAUSE;}
static void default_event_handler(int x) {(void)x;}
static bool podlink_host_connected(void) {return cable;}
static int usb_detect(void) {return cable ? USB_INSERTED : 0;}
static bool usb_confirm_pending(uint32_t e) {return cable && config_wait && e==3;}
static void usb_confirm_reply(uint32_t e,bool disk) {assert(usb_confirm_pending(e));config_wait=false;usb_replies++;disk_choices+=disk;}
#ifdef TEST_BASELINE
static void sleep(int ticks) {current_tick+=ticks;}
#endif
static uint32_t podlink_crc32(const unsigned char *d,unsigned int n)
{uint32_t crc=17;for(unsigned i=0;i<n;++i)crc=crc*31+d[i];return crc;}
static void mutex_init(struct mutex *m) {m->used=0;}
static void mutex_lock(struct mutex *m) {assert(!m->used++);}
static void mutex_unlock(struct mutex *m) {assert(m->used--==1);}
#ifndef TEST_FU76_SHEET
static void action_wait_for_release(void) {}
static void button_queue_post(int x, int y) {(void)x;(void)y;redraws++;}
static void backlight_on(void) {}
static void splashf(int ticks,const char *f,int s) {(void)ticks;(void)f;(void)s;splashes++;}
#endif
#ifndef TEST_REAL_PCM
static void pcmbuf_handoff_cancel(void) {fade=0;}
static void pcmbuf_handoff_hold(bool v) {pcm_hold=v;if (!v) pcm_end=false;}
static bool pcmbuf_handoff_ended(void) {return pcm_end;}
static void pcmbuf_handoff_fade(unsigned long t) {fade=t;captured=-1ul;}
#ifdef TEST_BASELINE
static unsigned long pcmbuf_handoff_fade_begin(void)
{return fade <= (unsigned long)current_tick ? fade : 0;}
#else
static unsigned long pcmbuf_handoff_at(void) {return captured;}
#endif
static unsigned long pcmbuf_handoff_position(unsigned long p) {return sample ? sample : p == -1ul ? local.elapsed : p;}
#endif
#define O_WRONLY 1
#define O_CREAT 2
#define O_APPEND 4
static int open(const char *path, int flags, int mode)
{assert(!strcmp(path,"/.rockbox/handoff-log.txt"));(void)flags;(void)mode;return 1;}
#define fdprintf(fd, ...) (assert(fd==1), snprintf(log_line,sizeof log_line,__VA_ARGS__))
static void close(int fd) {assert(fd==1);}
struct viewport { int x,y,width,height,flags; };
struct screen {
   int lcdwidth,lcdheight;
   int (*getcharheight)(void);
   void (*putsxy)(int,int,const char *);
   void (*set_viewport)(struct viewport *);
};
#ifdef TEST_FU76_SHEET
struct dialog { bool backlight_on; };
struct handoff_sheet {
    const char *title, *target;
    long accepted;
    unsigned long start;
    int reason;
    bool preparing, armed;
};
static int handoff_sheet_run(struct handoff_sheet *view, int (*advance)(int))
{
    int result;
    assert(dialog_depth++ == 0);
    if (view->reason)
    {
        ++splashes;
        current_tick += 2 * HZ;
        result = advance(DIALOG_CONTINUE);
    }
    else
    {
        ++prompts;
        frame += 2;
        redraws++;
        current_tick += 10000; /* indefinite question dwell */
        if (cancel_on_question) cable = false;
        result = advance(answer == ACTION_STD_OK ? DIALOG_ACCEPT : DIALOG_CANCEL);
    }
    assert(--dialog_depth == 0);
    return result;
}
#else
struct dialog_insets { int top,bottom; };
struct style {};
struct dialog;
struct dialog_callbacks {
    void (*measure)(struct dialog*,struct screen*,struct viewport*,void*);
    void (*draw)(struct dialog*,struct screen*,struct viewport*,void*);
    int (*on_action)(struct dialog*,int,void*);
};
struct dialog { bool backlight_on; struct style style; const struct dialog_callbacks *cb; void *data; };
static void dialog_get_insets(struct style *s,struct dialog_insets *in) {(void)s;in->top=in->bottom=4;}
static void dialog_draw_button(struct screen *s,struct style *style,int x,int y,int w,int h,const char *name,bool selected)
{(void)s;(void)style;(void)x;(void)y;(void)w;(void)h;if (!strcmp(name,"Hand off")) assert(selected);}
static void dialog_init(struct dialog *d,int i,void *p,void *q,const struct dialog_callbacks *cb,void *data)
{(void)i;(void)p;(void)q;*d=(struct dialog){.backlight_on=true,.cb=cb,.data=data};}
static int font_height(void) {return 13;}
static void puts_at(int x,int y,const char *s) {(void)x;(void)y;assert(*s);frame++;}
static void viewport(struct viewport *p) {assert(p->width>0);}
static int dialog_run(struct dialog *d,int ticks)
{
    assert(ticks>0);
    struct screen s={320,240,font_height,puts_at,viewport};
    struct viewport box={0};
    prompts++;
    assert(dialog_depth++==0);
    d->cb->measure(d,&s,&box,d->data);
    d->cb->draw(d,&s,&box,d->data);
    current_tick+=10000; /* a long idle question never times out */
    assert(d->cb->on_action(d,BUTTON_REDRAW,d->data)==DIALOG_CONTINUE);
    if (cancel_on_question) cable=false;
    int result=d->cb->on_action(d,answer,d->data);
    assert(--dialog_depth==0);
    return result;
}
#endif
'''

body = re.sub(r'^#include .*$', '', HANDOFF.read_text(), flags=re.M)
usb = ROOT / 'apps-ipod/widgets/usb_confirm.c'
widget = '\n'.join(function(usb, name) for name in ('remote_choice', 'confirm_action'))
scenario = r'''
static void feed(const char *msg)
{
    struct podlink_message m={PODLINK_TEXT,strlen(msg),(const unsigned char*)msg};
    assert(handoff_receive(&m));
}
static void host(const char *s)
{
    char b[128];snprintf(b,sizeof b,"HF\t1\t%lu\t%s",ident,s);
    feed(b);
}
static void dock(bool supported,int screen,int audio)
{
    cable=false;handoff_reset();handoff_step();
    active=false;online=true;
    cable=config_wait=true;activity=screen;status=audio;
    answer=ACTION_STD_MENU;cancel_on_question=0;sample=0;
    if (supported) feed("HF\t1");
}
static void open_wps(void)
{
    assert(handoff_prompt_wps(3));assert(phase==MATCH&&playing()&&handoff_hold_source());
    char msg[512];assert(handoff_next(msg,sizeof msg));
    assert(strstr(msg,"\t180000\tSong\tArtist\tAlbum"));
}
static void offer(void)
{
    host("offer\tRufin\t1");
    handoff_step();
}
static void arm(void)
{
    answer=ACTION_STD_OK;offer();assert(phase==PREPARE);
    char msg[512];assert(handoff_next(msg,sizeof msg) && strstr(msg,"\ta\t"));
    host("ping\t2\t987654");
    assert(handoff_next(msg,sizeof msg));
    assert(strstr(msg,"\t987654\t2\t"));
    char go[100];snprintf(go,sizeof go,"go\t%lu\t-1000\t12\t-5\t180\t20",current_tick+30);
    host(go);
    assert(handoff_next(msg,sizeof msg) && strstr(msg,"\tg\t"));
    handoff_step();
    assert(phase==ARMED&&fade==(unsigned long)current_tick+30);
}
int main(void)
{
    handoff_init();
    dock(false,ACTIVITY_WPS,AUDIO_STATUS_PLAY);
    assert(!handoff_prompt_wps(3) && config_wait && !handoff_hold_source());
    feed("HF\t2");
    assert(!handoff_capable());
    struct dialog d={.backlight_on=true};
    uint32_t epoch=3;
    assert(confirm_action(&d,ACTION_STD_OK,&epoch)==DIALOG_ACCEPT && disk_choices==1);

    dock(true,ACTIVITY_WPS,AUDIO_STATUS_PLAY|AUDIO_STATUS_PAUSE);
    assert(!handoff_prompt_wps(3) && config_wait && !remote_choice());
    dock(true,2,AUDIO_STATUS_PLAY);
    assert(!handoff_prompt_wps(3) && config_wait && remote_choice());
    assert(confirm_action(&d,ACTION_STD_NEXT,&epoch)==DIALOG_ACCEPT);
    assert(phase==MATCH && !config_wait && playing());
    offer();assert(phase==WAIT_END&&pcm_hold&&playing());
    strcpy(local.path,"/Manual-skip.mp3");
    assert(handoff_step()==0&&!active&&playing());
    pcm_end=true;
    assert(handoff_step()==2&&active&&stops==1&&!pcm_hold);
    assert(strstr(log_line,"track-end"));

    dock(true,ACTIVITY_WPS,AUDIO_STATUS_PLAY);
    open_wps();offer();
    assert(phase==WAIT_END && playing());
    status|=AUDIO_STATUS_PAUSE;
    assert(handoff_step()==2&&active&&stops==1);
    assert(strstr(log_line,"paused"));

    dock(true,ACTIVITY_WPS,AUDIO_STATUS_PLAY);
    open_wps();host("offer\tCider\t0");handoff_step();
    assert(phase==WAIT_END && playing() && splashes==1);
    assert(strstr(log_line,"not-found")&&strstr(log_line,"Cider"));

    dock(true,ACTIVITY_WPS,AUDIO_STATUS_PLAY);
    open_wps();arm();
    current_tick=fade+150;
    cable=false;handoff_reset();handoff_step();
    assert(!fade&&playing()&&stops==1&&!active);
    assert(strstr(log_line,"unplug"));
    dock(true,ACTIVITY_WPS,AUDIO_STATUS_PLAY);
    open_wps();arm();
    current_tick=fade;captured=sample=22000;host("at\t22012");handoff_next((char[80]){0},80);handoff_step();
    current_tick=fade+300;host("end");
    assert(handoff_step()==2&&active&&stops==2&&!fade);
    assert(strstr(log_line,"position_error_ms=12")&&strstr(log_line,"accepted"));

    dock(true,ACTIVITY_WPS,AUDIO_STATUS_PLAY);
    open_wps();arm();current_tick=fade;captured=sample=22000;host("at\t22151");handoff_next((char[80]){0},80);handoff_step();
    assert(phase==ARMED&&transport==VERIFIED&&playing()&&fade);
    current_tick=fade+300;host("end");
    assert(handoff_step()==2&&active&&strstr(log_line,"position_error_ms=151"));

    dock(true,ACTIVITY_WPS,AUDIO_STATUS_PLAY);
    open_wps();arm();current_tick=fade;captured=sample=22000;host("at\t23001");handoff_next((char[80]){0},80);handoff_step();
    assert(phase==WAIT_END&&playing()&&!fade&&strstr(log_line,"alignment"));

    for (int i=0;i<50;i++)
    {
        dock(true,ACTIVITY_WPS,AUDIO_STATUS_PLAY);
        open_wps();cancel_on_question=1;offer();
        assert(phase==IDLE&&dialog_depth==0&&playing()&&!active);
    }
    assert(prompts==56&&frame==112&&redraws>=56);
    puts("FU75: WPS and other-screen consent, old host, pause, EOF/skip, no match, fade, unplug and 50 dialog cycles");
}
'''
test_c = OUT / 'handoff.c'
test_c.write_text('#define TEST_FU76_SHEET\n' + ports + body + widget + scenario)
binary = OUT / 'handoff'
subprocess.run(['cc', '-std=gnu11', '-Wall', '-Wextra', '-Werror',
                '-fsanitize=address,undefined', '-fno-pie', '-no-pie',
                str(test_c), '-o', str(binary)], check=True)
subprocess.run([binary], check=True)

dialog = (ROOT / 'apps-ipod/widgets/dialog.c').read_text()
assert dialog.index('viewportmanager_theme_enable(i, true, &d->parent[i])') < dialog.index(
    'disp = d->cb->on_action(d, action, d->data)') < dialog.index('viewportmanager_theme_undo(i, false)')
