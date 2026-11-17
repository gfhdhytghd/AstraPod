"""Execute the production viewer and document with recording UI/source adapters.

The simulator supplies visual proof. This fixture checks local/remote dispatch,
page-cache paging, input during retargets, idle return and arena-denied cleanup.
"""
from pathlib import Path
import ast,re,subprocess,sys
r=Path(__file__).resolve().parents[2];o=Path(sys.argv[1]).resolve();o.mkdir(parents=True,exist_ok=True)
ns=ast.parse((r/'tools/tests/lyrics_document_test.py').read_text())
pre=next(ast.literal_eval(n.value) for n in ns.body if isinstance(n,ast.Assign) and any(isinstance(t,ast.Name) and t.id=='pre' for t in n.targets))
pre=pre.replace('static struct {int default_codepage;} global_settings;', '''static struct {int default_codepage,lyric_colour_mode,backlight_timeout,backlight_timeout_plugged,lyric_prev_opacity,lyric_next_opacity,lyric_lead;unsigned fg_color,bg_color;bool lyric_highlight,lyric_backlight,prevent_skip;} global_settings;''')
pre=pre.replace('length,id3v2len;', 'length,id3v2len,elapsed;')
strutil=(r/'apps-ipod/system/strutil.c').read_text()
read_line=re.search(r'^int read_line\(.*?^}',strutil,re.M|re.S).group()
pre=re.sub(r'^static int read_line\([^\n]+\n', lambda _: '#include <errno.h>\n'+read_line+'\n', pre, flags=re.M)
source=(r/'apps-ipod/viewers/lyric_viewer/lyric_viewer.c').read_text()
source=re.sub(r'^#include ".*\n','',source,flags=re.M)
doc=(r/'apps-ipod/viewers/lyric_viewer/lyrics.c').read_text();doc=re.sub(r'^#include ".*\n','',doc,flags=re.M)
h=(r/'apps-ipod/viewers/lyric_viewer/lyrics.h').read_text();h=re.sub(r'^#include "metadata.h".*\n','',h,flags=re.M)
tokens=sorted(set(re.findall(r'\bACTION_[A-Z_0-9]+',source)))
adapter=r'''
#define HAVE_USB_PODLINK
#define HZ 100
#define LCD_WIDTH 320
#define LCD_HEIGHT 240
#define LCD_WHITE 65535
#define LCD_BLACK 0
#define DRMODE_FG 1
#define BUTTON_REMOTE 0x8000
#define FONT_DIR "fonts"
#define ROCKBOX_DIR "."
#include "lyric_viewer.h"
#define fdprintf dprintf
#define USEC_TIMER mock_usec()
#define MIN(a,b) ((a)<(b)?(a):(b))
#define MAX(a,b) ((a)>(b)?(a):(b))
#define TIME_BEFORE(a,b) ((int32_t)((uint32_t)(a)-(uint32_t)(b))<0)
#define IS_SYSEVENT(x) ((x)>=1000)
#define SYS_USB_CONNECTED 1001
#define CONTEXT_WPS 1
#define SCREEN_MAIN 0
#define ACTIVITY_LYRICS 1
#define AUDIO_STATUS_PLAY 1
#define AUDIO_STATUS_PAUSE 2
#define GO_TO_ROOT 10
#define GO_TO_PREVIOUS 11
#define DEFAULT_SKIP_THRESH 3000
#define IS_DIACRITIC(x) false
#define FBADDR(x,y) (screen_pixels+(y)*320+(x))
typedef uint16_t fb_data;typedef unsigned ucschar_t;
struct frame_buffer_t {fb_data *fb_ptr;void*(*get_address_fn)(int,int);int stride,elems;};
struct viewport {struct frame_buffer_t *buffer;unsigned bg_pattern;int font,fg,width,height,flags;};
struct bitmap {int width,height;unsigned char *data;};
struct screen {int(*getuifont)(void);};
struct wps_state {struct mp3entry *id3,*nid3;int ff_rewind_count;bool paused;};
struct cider_row {char id[80],title[124];};
static uint32_t current_tick, precise_extra;
static uint32_t mock_usec(void){return current_tick*10000+precise_extra;}
static fb_data screen_pixels[320*240],app_area[3*1024*1024/sizeof(fb_data)];
static struct viewport default_vp,*vp=&default_vp;
static struct mp3entry playing;
static struct wps_state wps={.id3=&playing};
static struct font {int height;} f12={12},f18={18},f26={26};
static int calls,plan,loads,unloads,claims,releases,holds,activities,updates,pauses,seeks,skips,controls,remote_at,default_events;
static bool remote_source,deny_arena,inhibited,remote_key,playing_audio=true,page_missed;
static bool timed_fixture, wait_row, change_during_copy;
static struct cider_state remote_clock;
static int remote_opens, paused_width;
static int retarget_position, idle_updates, remote_actions, lead_boundary;
static int ui_font(void){return 18;}static struct screen screens[]={{ui_font}};
static struct font *font_get(int f){return f==12?&f12:f==26?&f26:&f18;}
static int font_load(const char *path){loads++;return strstr(path,"12-")?12:strstr(path,"26-")?26:18;}
static void font_unload(int f){(void)f;unloads++;}
static int font_get_width(struct font*f,unsigned ch){return ch>127?f->height:f->height/2;}
static const unsigned char *utf8decode(const unsigned char*p,ucschar_t *ch){*ch=*p++;if(*ch>=0xc0){int n=*ch>=0xf0?3:*ch>=0xe0?2:1;while(n--)*ch=(*ch<<6)|(*p++&63);}return p;}
static int font_getstringsize(const unsigned char*p,int*w,int*h,int f){*w=0;*h=f;while(*p){ucschar_t ch;p=utf8decode(p,&ch);*w+=font_get_width(font_get(f),ch);}return *w;}
static const char *text_wrap_next_punct(const char *p,int width,struct font*f,const char **end){int n=0;const char*s=p;while(*p&&*p!='\n'){ucschar_t ch;const char*q=(const char*)utf8decode((const unsigned char*)p,&ch);n+=font_get_width(f,ch);if(n>width&&p>s)break;p=q;}*end=p;return *p=='\n'?p+1:p;}
static unsigned color_blend(unsigned a,unsigned b,int p){unsigned rb=(((a&0xf81f)*(256-p)+(b&0xf81f)*p)>>8)&0xf81f;return rb|((((a&0x7e0)*(256-p)+(b&0x7e0)*p)>>8)&0x7e0);}
static void lcd_set_viewport(struct viewport*v){vp=v?v:&default_vp;}
static void lcd_setfont(int f){vp->font=f;}
static void lcd_set_drawmode(int m){(void)m;}
static void lcd_set_foreground(unsigned c){vp->fg=c;}
static void lcd_fillrect(int x,int y,int w,int h){for(int r=MAX(y,0);r<MIN(y+h,240);r++)for(int c=MAX(x,0);c<MIN(x+w,320);c++)(vp->buffer?vp->buffer->fb_ptr:screen_pixels)[r*320+c]=vp->fg;}
static void lcd_putsxy(int x,int y,const char*p){int w,h;font_getstringsize((const unsigned char*)p,&w,&h,vp->font);lcd_fillrect(x,y,w,h);}
static void fill_round_rect(struct screen*s,int x,int y,int w,int h,int radius){(void)s;(void)radius;lcd_fillrect(x,y,w,h);}
static void lcd_update(void){updates++;}
static void lcd_update_rect(int x,int y,int w,int h){assert(x>=0&&y>=0&&w>0&&h>0&&x+w<=320&&y+h<=240);updates++;}
static void sleep(int n){assert(n>=0);current_tick+=n+1;}
static void *app_try_claim_buffer(size_t*n,const char*s){assert(!strcmp(s,"lyrics"));if(deny_arena)return NULL;*n=sizeof app_area;claims++;return app_area;}
static void app_release_buffer(const char*s){assert(!strcmp(s,"lyrics"));releases++;}
static void skin_hold_scrolling(bool h){holds+=h?1:-1;assert(holds>=0);}
static bool skin_flush_inhibited(void){return inhibited;}
static void skin_inhibit_flush(bool v){inhibited=v;}
static unsigned art_generation;
static unsigned skin_albumart_gen(void){return art_generation;}
static void push_current_activity(int a){(void)a;activities++;}
static void pop_current_activity(void){activities--;}
static void viewportmanager_theme_enable(int s,bool b,struct viewport*v){(void)s;(void)b;*v=(struct viewport){0};}
static void viewportmanager_theme_undo(int s,bool b){(void)s;(void)b;}
static unsigned dynamic_colors_resolve(unsigned c){return c;}
static bool dynamic_colors_roles(unsigned*a,unsigned*b){(void)a;(void)b;return false;}
static bool art_present, cached_art;
static int raw_art_reads, art_handle=1;
static fb_data art_pixels[256];
static struct bitmap art={16,16,(unsigned char*)art_pixels};
static int skin_lyrics_art(void){return art_present?art_handle:-1;}
static bool wps_art_pin(int h,bool b){(void)b;return art_present&&h==art_handle;}
static bool wps_art_bitmap(int h,struct bitmap**b){raw_art_reads++;*b=&art;return art_present&&h==art_handle;}
enum { IMG_CLASSES_ALL, IMG_CLASSES_INPLACE };
struct img_filter { int text_lum; bool scrim; };
struct img_levels { int unused; };
static bool img_filter_compile(const char *spec,int allowed,struct img_filter *f)
{
    assert(!strcmp(spec,allowed==IMG_CLASSES_ALL?"blur12+scrim:ffffff+dither":"darker45+dither"));
    *f=(struct img_filter){.text_lum=255,.scrim=allowed==IMG_CLASSES_ALL};return true;
}
static bool skin_lyrics_copy_blur(fb_data *dst,int handle,const struct img_filter *f)
{
    assert(handle==art_handle&&f->scrim);
    if(!cached_art)return false;
    for(int i=0;i<320*240;i++)dst[i]=art_pixels[0];
    return true;
}
static void img_filter_measure(const fb_data *p,int w,int h,struct img_levels *levels)
{assert(p==art_pixels&&w==16&&h==16);levels->unused=0;}
static void img_filter_adapt(struct img_filter *f,const struct img_levels *levels,int avoid)
{assert(f->scrim&&avoid==255&&!levels->unused);}
static void img_filter_render(fb_data *dst,int w,int h,const fb_data *p,int sw,int sh,const struct img_filter *f)
{
    assert(f->scrim&&w==320&&h==320&&sw==16&&sh==16);
    for(int i=0;i<w*h;i++)dst[i]= i<40*w ? 0 : p[0];
    precise_extra+=4000;
}
static void img_filter_apply_banded(fb_data *dst,int w,int h,const struct img_filter *f)
{
    assert(w==320&&h==240&&!f->scrim);
    for(int i=0;i<w*h;i++)dst[i]=color_blend(0,dst[i],141);
    precise_extra+=1000;
}
static void backlight_set_timeout(int n){(void)n;}static void backlight_set_timeout_plugged(int n){(void)n;}
static void remote_cn(const char *id, unsigned position, unsigned rate)
{
    char record[192];
    int n=snprintf(record,sizeof record,"CN\t9\t%u\t1\t10000\t%u\t%u\t0\t0\t0\t%s\t-\t181818\tRemote\tArtist\tAlbum",
                   remote_clock.seq+1,position,rate,id);
    assert(cider_state_apply(&remote_clock,record,n,current_tick));
}
static struct wps_state *get_wps_state(void)
{
    if (remote_source && timed_fixture)
    {
        snprintf(playing.path,sizeof playing.path,"cider/%s",remote_clock.id);
        playing.elapsed=cider_position(&remote_clock,current_tick,HZ);
        playing.length=remote_clock.duration;
        wps.paused=!remote_clock.rate;
    }
    if (remote_source && !strncmp(playing.path,"cider/",6))
        lyrics_remote_track(playing.path+6,9,0,playing.length);
    return &wps;
}
static bool cider_lyrics_matches(const char *id,uint32_t generation,uint32_t library)
{
    return generation==9&&!library&&!strcmp(id,timed_fixture?remote_clock.id:playing.path+6);
}
static bool cider_active(void){return remote_source;}
static const char *cider_remote_name(void){return "Rufin";}
static int audio_status(void){return playing_audio?AUDIO_STATUS_PLAY:0;}
static struct mp3entry *audio_current_track(void){return playing_audio?&playing:NULL;}
static void wps_lyrics_backdrop(void){for(int i=0;i<320*240;i++)screen_pixels[i]=0x1234;}
static void wps_do_playpause(bool b){(void)b;pauses++;wps.paused=!wps.paused;}
static void wps_lyrics_skip(int d){skips++;assert(d==1);char*leaf=strrchr(playing.path,'/');assert(leaf);strcpy(leaf+1,"other.wav");playing.title="Second";playing.artist="Artist2";playing.elapsed=0;}
static void wps_lyrics_seek(int a,void(*draw)(void)){(void)a;seeks++;wps.ff_rewind_count=500;draw();wps.ff_rewind_count=0;}
static void cider_control(const char*c,int v){assert(v==0);assert(!strcmp(c,"toggle")||!strcmp(c,"next")||!strcmp(c,"previous"));controls++;}
static void cider_page_open(const char*q)
{
    static char previous[144];
    assert(strstr(q,"now/lyrics?nonce=")&&strstr(q,"&lrc=1&track="));
    if(!strcmp(previous,q))return;
    strcpy(previous,q);
    if (timed_fixture) assert(strstr(q,remote_clock.id));
    remote_at=0;
    page_missed=false;
    remote_opens++;
}
static int cider_page_total(void)
{
    if (!timed_fixture) return 75;
    return wait_row || !strcmp(remote_clock.id,"two") ? 1 : 3;
}
static bool cider_page_row(int i,struct cider_row*r)
{
    assert(i==remote_at);
    strcpy(r->id,"0");
    if (!timed_fixture)
    {
        if (i==50&&!page_missed){page_missed=true;return false;}
        snprintf(r->title,sizeof r->title,"Remote row %d",i);
    }
    else if (wait_row)
    {
        strcpy(r->id,"lyric-wait");
        r->title[0]=0;
        return true;
    }
    else if (!strcmp(remote_clock.id,"two"))
    {
        strcpy(r->id,"lrc1");
        strcpy(r->title,"[00:00.00]Current track only");
    }
    else
    {
        if (i==1&&!page_missed){page_missed=true;return false;}
        strcpy(r->id,i==0?"lrc1+":"lrc1");
        strcpy(r->title,i==0?"[00:00.00]<00:00.00>星 ":i==1?"<00:01.00>光<00:02.00>":"[00:02.00]Second focus");
        if (i==2 && change_during_copy) remote_cn("two",0,0);
    }
    remote_at++;
    return true;
}
static int get_action_statuscode(int*b){if(b)*b=remote_key?BUTTON_REMOTE:0;return 0;}
static int default_event_handler(int a){default_events++;return a;}
static int get_action(int c,int timeout);
'''
# Avoid libc sleep's POSIX signature in this host adapter.
pre=pre.replace('#include <unistd.h>','#include <unistd.h>\n#include <limits.h>\n#define sleep ui_sleep')
post=r'''
static int get_action(int c,int timeout){assert(c==CONTEXT_WPS&&timeout>0&&timeout<=50);current_tick+=timeout;calls++;assert(calls<50);
 if(plan==0){
  if(calls==1){assert(lv.mode.focus==0);return ACTION_WPS_VOLUP;}
  if(calls==2){assert(lv.mode.manual&&lv.mode.focus==1);retarget_position=lyrics_motion_at(&lv.scroll,current_tick);return ACTION_WPS_VOLUP;}
  if(calls==3){assert(lv.scroll.from==retarget_position&&lv.mode.focus==2);current_tick+=400;playing.elapsed=9000;return ACTION_NONE;}
  if(calls==4){assert(!lv.mode.manual&&lv.mode.focus==2);return ACTION_WPS_PLAY;}
  if(calls==5)return ACTION_WPS_SEEKFWD;
  if(calls==6)return ACTION_WPS_SKIPNEXT;
  if(calls==7){assert(!lyrics_timed()&&lyrics_count()==3&&lv.track_fade);assert(!memcmp(lv_frame(),lv.snapshot,LV_FRAME));current_tick+=20;return ACTION_NONE;}
  assert(!lv.track_fade&&lv.mode.focus==0&&strstr(lv.title,"Second"));return ACTION_WPS_BROWSE;
 }
 if(plan==1){remote_key=true;
  if(calls==1){
    assert(lyrics_loaded()&&!lv.pending&&lyrics_count()>=16&&lyrics_count()<75);
    assert(perf.type[LV_ENTRY].count&&perf.phase[LV_GLYPHS].count);
  }
  if(lyrics_count()<75)return ACTION_NONE;
  assert(lyrics_loaded()&&remote_doc.done&&!lyrics_timed());
  switch(++remote_actions){
   case 1:return ACTION_WPS_VOLDOWN;
   case 2:assert(lv.mode.manual&&lv.mode.focus==1);return ACTION_WPS_PLAY;
   case 3:return ACTION_WPS_SKIPNEXT;
   case 4:return ACTION_WPS_SKIPPREV;
   default:return ACTION_WPS_LYRICS;
  }
 }
 if(plan==3){
  if(calls==1){assert(lv.pending&&remote_doc.retry&&!lyrics_loaded());wait_row=false;current_tick+=50;return ACTION_NONE;}
  if(lv.pending)return ACTION_NONE;
  assert(lyrics_timed()&&lyrics_loaded()&&lyrics_count()==2&&!strcmp(lyrics_text(0),"星 光"));
  switch(++remote_actions){
   case 1:assert(!lv.paused&&lv.mode.focus==0);paused_width=lv_sung_width(0,26);current_tick+=20;return ACTION_NONE;
   case 2:assert(lv_sung_width(0,26)>=paused_width&&remote_clock.seq==1);
     assert(!lv.art_ready&&!perf.builds);
     art_present=true;art_handle=7;art_pixels[0]=0x07e0;art_generation++;
     remote_cn("one",850,0);return ACTION_NONE;
   case 3:assert(lv.paused&&lv.elapsed==850);paused_width=lv_sung_width(0,26);current_tick+=120;return ACTION_NONE;
   case 4:assert(lv.elapsed==850&&lv_sung_width(0,26)==paused_width);
     assert(lv.remote&&lv.art_ready&&perf.builds==1);
     remote_cn("one",1799,0);return ACTION_NONE;
   case 5:assert(lv.mode.focus==0);remote_cn("one",1800,0);return ACTION_NONE;
   case 6:assert(lv.elapsed==1800&&lv.mode.focus==1&&lv.scroll.to==256);return ACTION_WPS_VOLDOWN;
   case 7:assert(lv.mode.manual&&lv.mode.focus==0&&lv.pill.to==256);current_tick+=400;return ACTION_NONE;
   default:assert(!lv.mode.manual&&lv.mode.focus==1&&lv.pill.to==0);return ACTION_WPS_LYRICS;
  }
 }
 if(plan==4){
  if(strcmp(lv.track,"cider/two")){assert(!lyrics_loaded());return ACTION_NONE;}
  assert(!lv.pending&&lyrics_count()==1&&!strcmp(lyrics_text(0),"Current track only"));
  assert(lv.elapsed==0&&lv.mode.focus==0);return ACTION_WPS_LYRICS;
 }
 if(plan==6){
   assert(lyrics_time(1)==4000&&lv.old_progress==(int)(playing.elapsed*320/playing.length));
   if(calls==1){assert(lv.mode.focus==0);playing.elapsed=lead_boundary;return ACTION_NONE;}
   if(calls==2){assert(lv.mode.focus==1&&lv_word_width(1,26,NULL)==0);playing.elapsed+=500;return ACTION_NONE;}
   assert(lv_word_width(1,26,NULL)>0);
   return ACTION_WPS_LYRICS;
 }
 if(plan==5){
   if(calls==1){
     assert(lv.art_ready&&perf.cached==1&&!perf.builds&&raw_art_reads==0);
     assert(lv.background[120*320]==color_blend(0,0xffff,141));
     assert(lv.background[0]<lv.background[120*320]);
     idle_updates=updates;art_handle=4;art_generation++;
     return ACTION_NONE;
   }
   if(calls==2){
     assert(updates==idle_updates&&perf.cached==1&&!perf.builds);
     cached_art=false;art_handle=2;art_pixels[0]=0x001f;art_generation++;
     return ACTION_WPS_SKIPNEXT;
   }
   if(calls==3){
     assert(lv.art_ready&&perf.builds==1&&perf.build==5000&&raw_art_reads==1&&lv.track_fade);
     assert(lv.background[40*320]==color_blend(0,0x001f,141));
     assert(!memcmp(lv_frame(),lv.snapshot,LV_FRAME));
     current_tick+=25;return ACTION_NONE;
   }
   if(calls==4){
     assert(lv.art_ready&&!lv.track_fade&&perf.builds==1&&raw_art_reads==1);
     art_present=false;strcpy(strrchr(playing.path,'/')+1,"uncovered.wav");return ACTION_NONE;
   }
   assert(!lv.art_ready&&lv.track_fade&&perf.builds==1);
   return ACTION_WPS_MENU;
 }
 assert(!lv.snapshot&&!lv.claimed&&!lyrics_loaded());return SYS_USB_CONNECTED;
}
int main(int argc,char**argv){assert(argc==2);assert(!chdir(argv[1]));
 assert(lv_mix(0,65535,128)==0x7bef);
 assert(lv_mix(0,0xf800,128)==0x7800&&lv_mix(0,0x07e0,128)==0x03e0&&lv_mix(0,0x001f,128)==0x000f);
 art_present=true;for(int i=0;i<256;i++)art_pixels[i]=i<16?0x001f:0xf800;
 art_present=false;
 assert(lv_mix(0x1234,0xabcd,0)==0x1234&&lv_mix(0x1234,0xabcd,256)==0xabcd);
 FILE*f=fopen("song.lrc","w");assert(f);fputs("[00:00.00]One\n[00:04.00]<00:04.00>Two <00:06.00>words\n[00:08.00]Three\n",f);fclose(f);
 f=fopen("other.txt","w");assert(f);fputs("New first line\nNew second line\n中文第三行",f);fclose(f);
 snprintf(playing.path,sizeof playing.path,"%s/song.wav",argv[1]);playing.title="Title";playing.artist="Artist";playing.length=60000;
 global_settings.lyric_lead=200;global_settings.fg_color=65535;global_settings.lyric_highlight=true;global_settings.lyric_backlight=true;global_settings.lyric_prev_opacity=global_settings.lyric_next_opacity=55;
 wps_lyrics_backdrop();assert(lyric_viewer()==GO_TO_PREVIOUS);assert(pauses==1&&seeks==1&&skips==1);
 assert(claims==1&&releases==1&&loads==3&&unloads==3&&!holds&&!activities&&!inhibited&&!lyrics_loaded());
 plan=1;calls=0;remote_source=true;playing.elapsed=0;strcpy(playing.path,"cider/song");
 get_wps_state();current_tick+=3;assert(lyrics_remote_step());assert(lyrics_remote_step());
 assert(lyrics_loaded()&&lyrics_count()==16&&remote_doc.first_ms==30&&!remote_doc.done);
 assert(lyric_viewer()==GO_TO_PREVIOUS&&controls==3&&remote_at==75);
 assert(lyrics_loaded()&&remote_doc.done&&remote_doc.complete_ms>=remote_doc.first_ms);
 lyrics_remote_cancel();
 assert(claims==2&&releases==2&&loads==6&&unloads==6&&!holds&&!activities&&!inhibited);
 plan=2;calls=0;remote_source=false;playing_audio=false;wps.id3=NULL;deny_arena=true;
 assert(lyric_viewer()==GO_TO_ROOT&&default_events==1&&releases==2&&!holds&&!activities&&!inhibited);
 deny_arena=false;playing_audio=true;wps.id3=&playing;
 plan=5;calls=0;art_present=cached_art=true;art_pixels[0]=0xffff;
 snprintf(playing.path,sizeof playing.path,"%s/song.wav",argv[1]);
 assert(lyric_viewer()==GO_TO_PREVIOUS&&releases==3&&!lv_state);
 FILE *metrics=fopen("lyrics-perf.txt","r");assert(metrics);
 char report[1024];struct lyrics_perf_history log=lyrics_perf_scan(fileno(metrics));
 assert(log.count==4&&log.session==4&&log.latest>0);
 assert(!fseek(metrics,log.latest,SEEK_SET));
 size_t len=fread(report,1,sizeof(report)-1,metrics);fclose(metrics);report[len]=0;
 assert(!strncmp(report,"Session 4\nUptime (s): ",22)&&strstr(report,"Duration (ms): "));
 assert(strstr(report,"Source: Local\nTrack: Untimed\n"));
 assert(strstr(report,"blur reuse/builds: 1/1\n")&&strstr(report,"blur max ms: 5\n"));
 assert(strstr(report,"SELECT to first ms: 31\n")&&strstr(report,"animation fps: "));
 assert(strstr(report,"restore: ")&&strstr(report,"text/glyph: ")&&strstr(report,"blend: ")&&strstr(report,"LCD: ")&&strstr(report,"wait: "));
 assert(strstr(report,"entry: ")&&strstr(report,"exit: ")&&strstr(report,"track change: ")&&strstr(report,"CPU MHz"));
 timed_fixture=true;remote_source=true;remote_key=false;
 const char ca[]="CA\t9\t1\tOK";
 assert(cider_state_apply(&remote_clock,ca,sizeof ca-1,current_tick));
 remote_cn("one",0,1000);
 plan=3;calls=0;remote_actions=0;wait_row=true;
 assert(lyric_viewer()==GO_TO_PREVIOUS&&remote_opens==3);
 assert(lyrics_loaded()&&!strcmp(lyrics_text(1),"Second focus"));
 lyrics_remote_cancel();
 remote_cn("one",2100,0);
 plan=4;calls=0;change_during_copy=true;
 assert(lyric_viewer()==GO_TO_PREVIOUS&&remote_opens==5&&releases==5);
 lyrics_remote_cancel();
 remote_source=false;plan=6;art_present=false;
 snprintf(playing.path,sizeof playing.path,"%s/song.wav",argv[1]);
 int leads[]={200,0,-200};
 for(int i=0;i<3;i++){
   calls=0;global_settings.lyric_lead=leads[i];lead_boundary=4000-leads[i];playing.elapsed=lead_boundary-1;
   assert(lyric_viewer()==GO_TO_PREVIOUS);
 }
 assert(updates>40);
 struct lv_state recorder={.entry_at=UINT32_MAX-1999,.first_pending=true};lv_state=&recorder;
 lv.length=60000;lv.elapsed=100;assert(lv_lyric_elapsed()==0);
 global_settings.lyric_lead=200;lv.elapsed=59999;assert(lv_lyric_elapsed()==60000);
 current_tick=0;precise_extra=1000;
 lv_phase_end(LV_RESTORE,500);lv_record(0,LV_STEADY,false);
 for(int i=1;i<95;i++)lv_record(0,LV_STEADY,false);
 for(int i=0;i<5;i++){precise_extra=41000;lv_record(0,LV_STEADY,true);}
 assert(perf.total.count==100&&perf.dropped==5&&perf.total.maximum==41000&&perf.first==43000);
 assert(lv_p95(&perf.total)==1000&&perf.phase[LV_RESTORE].sum==500);
 precise_extra=42000;lv_record(41000,LV_ENTRY,true);
 precise_extra=100000;lv_record(99000,LV_ENTRY,true);
 assert(perf.gaps==1&&perf.gap_time==58000&&perf.dropped==6);
 assert(perf.type[LV_ENTRY].count==2&&perf.type[LV_STEADY].count==100);
 test_perf_history();
 puts("PASS production viewer: local/remote lyrics, art cache/cold/no art/late remote art, dirty idle, recorder and close log, track fade, arena and USB cleanup");
}
'''
debug_menu=(r/'apps-ipod/screens/system/debug_menu.c').read_text()
debug_view=re.search(r'^static bool dbg_lyrics_perf\(void\).*?^}',debug_menu,re.M|re.S).group()
history_fixture=r'''
#include <stdarg.h>
struct simplelist_info { bool scroll_all; };
static char shown[1024];
static void simplelist_info_init(struct simplelist_info *info, const char *title, int count, void *data)
{
    assert(!strcmp(title, "Lyrics perf") && !count && !data);
    info->scroll_all = false;
    shown[0] = 0;
}
static void simplelist_addline(const char *fmt, ...)
{
    size_t used = strlen(shown);
    va_list args;
    va_start(args, fmt);
    int n = vsnprintf(shown + used, sizeof shown - used, fmt, args);
    va_end(args);
    assert(n >= 0 && used + n + 1 < sizeof shown);
    strcat(shown, "\n");
}
static bool simplelist_show_list(struct simplelist_info *info)
{
    assert(info->scroll_all);
    return false;
}
''' + debug_view + r'''

static struct lyrics_perf_history scan_path(const char *path)
{
    int fd = open(path, O_RDONLY);
    struct lyrics_perf_history result = lyrics_perf_scan(fd);
    if (fd >= 0) close(fd);
    return result;
}

static void test_perf_history(void)
{
    assert(scan_path(LYRICS_PERF_PATH).count == 9);
    assert(!dbg_lyrics_perf());
    assert(strstr(shown, "Sessions kept: 9\nSession 9\n"));
    assert(strstr(shown, "Source: Local\nTrack: Timed\n"));
    assert(!strstr(shown, "Session 8") && !strstr(shown, LYRICS_PERF_END));

    /* Recover numbering after reboot and ignore a partially written visit. */
    int fd = open(LYRICS_PERF_PATH, O_WRONLY | O_APPEND);
    assert(fd >= 0 && dprintf(fd, "\nSession 10\nUptime (s): 1") > 0 && !close(fd));
    lv_perf_session = 0;
    assert(!dbg_lyrics_perf() && strstr(shown, "Sessions kept: 9\nSession 9\n"));
    lv.remote = true;
    lv_perf_save();
    assert(!dbg_lyrics_perf() && strstr(shown, "Sessions kept: 10\nSession 10\n"));
    assert(strstr(shown, "Source: Remote / Rufin\n"));

    /* Enough real appends to replace the archive repeatedly. */
    uint32_t previous_archive = 0;
    int rotations = 0;
    struct stat current_size, archive_size;
    for (uint32_t id = 11; id <= 250; id++)
    {
        lv_perf_save();
        struct lyrics_perf_history current = scan_path(LYRICS_PERF_PATH);
        struct lyrics_perf_history archive = scan_path(LYRICS_PERF_ARCHIVE);
        assert(current.session == id);
        assert(!stat(LYRICS_PERF_PATH, &current_size));
        if (!stat(LYRICS_PERF_ARCHIVE, &archive_size))
        {
            assert(archive_size.st_size <= LYRICS_PERF_FILE_LIMIT);
            if (archive.session != previous_archive)
            {
                assert(archive.session == id - 1 && current.count == 1);
                previous_archive = archive.session;
                rotations++;
            }
        }
        assert(current_size.st_size <= LYRICS_PERF_FILE_LIMIT);
    }
    assert(rotations >= 2);
    struct lyrics_perf_history archive = scan_path(LYRICS_PERF_ARCHIVE);
    struct lyrics_perf_history current = scan_path(LYRICS_PERF_PATH);
    assert(archive.count + current.count < 250);
    assert(!dbg_lyrics_perf());
    char expected[64];
    snprintf(expected, sizeof expected, "Sessions kept: %u\nSession 250\n",
             archive.count + current.count);
    assert(strstr(shown, expected));

    /* A legacy single-visit file remains readable and keeps its place. */
    assert(!unlink(LYRICS_PERF_ARCHIVE));
    FILE *legacy = fopen(LYRICS_PERF_PATH, "w");
    assert(legacy && fputs("frames: 12\ntrack change: 0 0/0/0\n", legacy) >= 0);
    assert(!fclose(legacy));
    lv_perf_session = 0;
    assert(!dbg_lyrics_perf() && strstr(shown, "Sessions kept: 1\nframes: 12\n"));
    lv_perf_save();
    assert(!dbg_lyrics_perf() && strstr(shown, "Sessions kept: 2\nSession 2\n"));
    puts("PASS performance history: append, restart, partial tail, rotation, count and legacy visit");
}
'''
p=o/'viewer.c';p.write_text(pre+h+doc+'\n#include "lyrics_motion.h"\n#include "cider_model.h"\nenum {'+','.join(tokens)+'};\n'+adapter+source+history_fixture+post)
playback=r/'apps-ipod/screens/playback'
subprocess.run(['cc','-m32','-std=gnu11','-Wall','-Wextra','-Werror','-Wno-unused-function',
                '-g','-fsanitize=address,undefined','-fno-pie','-no-pie','-ffunction-sections','-Wl,--gc-sections',
                '-I'+str(r/'apps-ipod/viewers/lyric_viewer'),'-I'+str(playback),
                '-I'+str(next(r.glob('firmware/**/podlink.h')).parent),
                str(p),str(r/'apps-ipod/viewers/lyric_viewer/lyrics_motion.c'),
                str(playback/'cider_model.c'),'-o',str(o/'viewer')],check=True)
subprocess.run([str(o/'viewer'),str(o)],check=True)
