"""Execute the production viewer and document with recording UI/source adapters.

The simulator supplies visual proof. This fixture checks local/remote dispatch,
page-cache paging, input during retargets, idle return and arena-denied cleanup.
"""
from pathlib import Path
import ast,re,subprocess,sys
r=Path(__file__).resolve().parents[2];o=Path(sys.argv[1]).resolve();o.mkdir(parents=True,exist_ok=True)
ns=ast.parse((r/'tools/tests/lyrics_document_test.py').read_text())
pre=next(ast.literal_eval(n.value) for n in ns.body if isinstance(n,ast.Assign) and any(isinstance(t,ast.Name) and t.id=='pre' for t in n.targets))
pre=pre.replace('static struct {int default_codepage;} global_settings;', '''static struct {int default_codepage,lyric_colour_mode,backlight_timeout,backlight_timeout_plugged,lyric_prev_opacity,lyric_next_opacity;unsigned fg_color,bg_color;bool lyric_highlight,lyric_backlight,prevent_skip;} global_settings;''')
pre=pre.replace('length,id3v2len;', 'length,id3v2len,elapsed;')
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
struct viewport {struct frame_buffer_t *buffer;unsigned bg_pattern;int font,fg;};
struct bitmap {int width,height;unsigned char *data;};
struct screen {int(*getuifont)(void);};
struct wps_state {struct mp3entry *id3,*nid3;int ff_rewind_count;bool paused;};
struct cider_row {char title[124];};
static uint32_t current_tick;
static fb_data screen_pixels[320*240],app_area[320*240*3];
static struct viewport default_vp,*vp=&default_vp;
static struct mp3entry playing;
static struct wps_state wps={.id3=&playing};
static struct font {int height;} f12={12},f18={18},f26={26};
static int calls,plan,loads,unloads,claims,releases,holds,activities,updates,pauses,seeks,skips,controls,remote_at,default_events;
static bool remote_source,deny_arena,inhibited,remote_key,playing_audio=true,page_missed;
static int retarget_position;
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
static void sleep(int n){assert(n>0);current_tick+=n;}
static void *app_try_claim_buffer(size_t*n,const char*s){assert(!strcmp(s,"lyrics"));if(deny_arena)return NULL;*n=sizeof app_area;claims++;return app_area;}
static void app_release_buffer(const char*s){assert(!strcmp(s,"lyrics"));releases++;}
static void skin_hold_scrolling(bool h){holds+=h?1:-1;assert(holds>=0);}
static bool skin_flush_inhibited(void){return inhibited;}
static void skin_inhibit_flush(bool v){inhibited=v;}
static unsigned skin_albumart_gen(void){return 0;}
static void push_current_activity(int a){(void)a;activities++;}
static void pop_current_activity(void){activities--;}
static void viewportmanager_theme_enable(int s,bool b,struct viewport*v){(void)s;(void)b;*v=(struct viewport){0};}
static void viewportmanager_theme_undo(int s,bool b){(void)s;(void)b;}
static unsigned dynamic_colors_resolve(unsigned c){return c;}
static bool dynamic_colors_roles(unsigned*a,unsigned*b){(void)a;(void)b;return false;}
static bool art_present;static fb_data art_pixels[256];static struct bitmap art={16,16,(unsigned char*)art_pixels};
static int wps_art_handle(int s){(void)s;return art_present?1:-1;}
static bool wps_art_pin(int h,bool b){(void)h;(void)b;return art_present;}
static bool wps_art_bitmap(int h,struct bitmap**b){(void)h;*b=&art;return art_present;}
static void backlight_set_timeout(int n){(void)n;}static void backlight_set_timeout_plugged(int n){(void)n;}
static struct wps_state *get_wps_state(void){return &wps;}
static bool cider_active(void){return remote_source;}
static int audio_status(void){return playing_audio?AUDIO_STATUS_PLAY:0;}
static struct mp3entry *audio_current_track(void){return playing_audio?&playing:NULL;}
static void wps_lyrics_backdrop(void){for(int i=0;i<320*240;i++)screen_pixels[i]=0x1234;}
static void wps_do_playpause(bool b){(void)b;pauses++;wps.paused=!wps.paused;}
static void wps_lyrics_skip(int d){skips++;assert(d==1);char*leaf=strrchr(playing.path,'/');assert(leaf);strcpy(leaf+1,"other.wav");playing.title="Second";playing.artist="Artist2";playing.elapsed=0;}
static void wps_lyrics_seek(int a,void(*draw)(void)){(void)a;seeks++;wps.ff_rewind_count=500;draw();wps.ff_rewind_count=0;}
static void cider_control(const char*c,int v){assert(v==0);assert(!strcmp(c,"toggle")||!strcmp(c,"next")||!strcmp(c,"previous"));controls++;}
static void cider_page_open(const char*q){assert(strstr(q,"now/lyrics?nonce="));remote_at=0;page_missed=false;}
static int cider_page_total(void){return 75;}
static bool cider_page_row(int i,struct cider_row*r){assert(i==remote_at);if(i==50&&!page_missed){page_missed=true;return false;}snprintf(r->title,sizeof r->title,"Remote row %d",i);remote_at++;return true;}
static int get_action_statuscode(int*b){if(b)*b=remote_key?BUTTON_REMOTE:0;return 0;}
static int default_event_handler(int a){default_events++;return a;}
static int get_action(int c,int timeout);
'''
# Avoid libc sleep's POSIX signature in this host adapter.
pre=pre.replace('#include <unistd.h>','#include <unistd.h>\n#define sleep ui_sleep')
post=r'''
static int get_action(int c,int timeout){assert(c==CONTEXT_WPS&&timeout>0&&timeout<=50);current_tick+=timeout;calls++;assert(calls<20);
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
  if(calls==1){assert(lv.pending&&!lyrics_loaded()&&remote_at==50);return ACTION_NONE;}
  assert(lyrics_loaded()&&lyrics_count()==75&&!lyrics_timed());
  if(calls==2)return ACTION_WPS_VOLDOWN;
  if(calls==3){assert(lv.mode.manual&&lv.mode.focus==1);current_tick+=500;return ACTION_NONE;}
  if(calls==4){assert(lv.mode.manual&&lv.mode.focus==1&&lv.pill.to==0);return ACTION_WPS_PLAY;}
  if(calls==5)return ACTION_WPS_SKIPNEXT;
  if(calls==6)return ACTION_WPS_SKIPPREV;
  return ACTION_WPS_MENU;
 }
 assert(!lv.snapshot&&!lv.claimed&&!lyrics_loaded());return SYS_USB_CONNECTED;
}
int main(int argc,char**argv){assert(argc==2);assert(!chdir(argv[1]));
 assert(lv_mix(0,65535,128)==0x7bef);
 assert(lv_mix(0,0xf800,128)==0x7800&&lv_mix(0,0x07e0,128)==0x03e0&&lv_mix(0,0x001f,128)==0x000f);
 art_present=true;for(int i=0;i<256;i++)art_pixels[i]=i<16?0x001f:0xf800;
 unsigned dominant;assert(lv_art_colour(&dominant)&&dominant==0xf800);art_present=false; /* no red/blue channel bleed */
 assert(lv_mix(0x1234,0xabcd,0)==0x1234&&lv_mix(0x1234,0xabcd,256)==0xabcd);
 FILE*f=fopen("song.lrc","w");assert(f);fputs("[00:00.00]One\n[00:04.00]<00:04.00>Two <00:06.00>words\n[00:08.00]Three\n",f);fclose(f);
 f=fopen("other.txt","w");assert(f);fputs("New first line\nNew second line\n中文第三行",f);fclose(f);
 snprintf(playing.path,sizeof playing.path,"%s/song.wav",argv[1]);playing.title="Title";playing.artist="Artist";playing.length=60000;
 global_settings.fg_color=65535;global_settings.lyric_highlight=true;global_settings.lyric_backlight=true;global_settings.lyric_prev_opacity=global_settings.lyric_next_opacity=55;
 wps_lyrics_backdrop();assert(lyric_viewer()==GO_TO_PREVIOUS);assert(pauses==1&&seeks==1&&skips==1&&lv.mode.focus==0);
 assert(claims==1&&releases==1&&loads==3&&unloads==3&&!holds&&!activities&&!inhibited&&!lyrics_loaded());
 plan=1;calls=0;remote_source=true;playing.elapsed=0;strcpy(playing.path,"cider/song");
 assert(lyric_viewer()==GO_TO_PREVIOUS&&controls==3&&remote_at==75&&lv.mode.focus==1);
 assert(claims==2&&releases==2&&loads==6&&unloads==6&&!holds&&!activities&&!inhibited);
 plan=2;calls=0;remote_source=false;playing_audio=false;wps.id3=NULL;deny_arena=true;
 assert(lyric_viewer()==GO_TO_ROOT&&default_events==1&&releases==2&&!holds&&!activities&&!inhibited);
 assert(updates>40);puts("PASS production viewer: local/remote toggle, track cross-fade, interrupted scroll, timed idle return, 75-row asynchronous text, untimed manual retention, transports, arena-denied animated entry/exit and USB cleanup");
}
'''
p=o/'viewer.c';p.write_text(pre+h+doc+'\n#include "lyrics_motion.h"\nenum {'+','.join(tokens)+'};\n'+adapter+source+post)
subprocess.run(['cc','-m32','-std=gnu11','-Wall','-Wextra','-Werror','-Wno-unused-function','-g','-fsanitize=address,undefined','-fno-pie','-no-pie','-I'+str(r/'apps-ipod/viewers/lyric_viewer'),str(p),str(r/'apps-ipod/viewers/lyric_viewer/lyrics_motion.c'),'-o',str(o/'viewer')],check=True)
subprocess.run([str(o/'viewer'),str(o)],check=True)
