"""FU39: real page parser/provider/caption cache, delayed rows without navigation."""
from pathlib import Path
import re, subprocess, sys
R = Path(__file__).resolve().parents[2]
O = Path(sys.argv[1]).resolve(); O.mkdir(parents=True, exist_ok=True)
carousel = Path(sys.argv[2]).read_text() if len(sys.argv) > 2 else (R/'apps-ipod/screens/covers/carousel.c').read_text()
provider = (R/'apps-ipod/screens/covers/cider_covers.c').read_text()
def fn(source, name):
    m = re.search(r'(?m)^(?:static\s+)?(?:inline\s+)?[\w *]+\b'+name+r'\([^;{}]*\)\s*\{', source)
    assert m, name
    e = m.end(); depth = 1
    while depth:
        depth += (source[e] == '{') - (source[e] == '}'); e += 1
    return source[m.start():e]
loop = fn(carousel, 'album_covers_loop')
if 'static bool update_caption_version(' in carousel:
    update = fn(carousel, 'update_caption_version')
    check = 'if (update_caption_version(&seen_caption_version))\n            caption_owed = true;'
    assert check in loop
    assert loop.index('if (model->tick) model->tick();') < loop.index(check) < loop.index('if (caption_owed && !art_owed')
    assert 'unsigned int seen_caption_version = 0;' in loop
else:
    # Execute the old loop's missing notification to prove the same fixture fails.
    update = 'static bool update_caption_version(unsigned int *seen) {(void)seen;return false;}'
assert 'if (!dark && (art_owed || caption_owed)' in loop
assert 'current_tick - last_frame_tick >= PF_FRAME_TICKS' in loop
assert 'get_custom_action(CONTEXT_PLUGIN, timeout, get_context_map)' in loop
quick = loop[loop.index('        case ACTION_STD_QUICKSCREEN:'):loop.index('        case PF_MENU:')]
pre = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "screens/playback/cider_library.h"
#define HZ 100
#define MIN(a,b) ((a)<(b)?(a):(b))
#define MAX(a,b) ((a)>(b)?(a):(b))
#define TIME_BEFORE(a,b) ((long)((a)-(b))<0)
#define MAX_PATH 260
#define TEXT_FIT_BUF 260
#define ALBUM_NAME_HIDE 0
#define ALBUM_AND_ARTIST_TOP 3
#define ALBUM_AND_ARTIST_BOTTOM 4
#define ALBUM_YEAR_BEFORE 1
#define ALBUM_YEAR_AFTER 2
#define PF_SCROLL_ALBUM 0
#define PF_SCROLL_ARTIST 1
#define SCREEN_MAIN 0
#define ACTION_NONE 0
#define ACTION_STD_QUICKSCREEN 1
#define ACTION_WPS_QUICKSCREEN 2
#define ACTION_STD_CANCEL 3
#define CONTEXT_STD 0
#define QUICKSCREEN_IN_USB 99
#define GO_TO_PREVIOUS -1
#define GO_TO_ROOT -2
#define AGENTS_NAV_NONE 0
#define CAROUSEL_REMOTE_RETURN -3
#define LANG_CAROUSEL_NO_ALBUMS 0
#define ID2P(n) "no albums"
#define DRM default_draw_mode
#define DRM_DEFAULT 0
#define DRMODE_FG 1
#define Icon_NOICON 0
struct viewport {int n;};struct pf_caption {int y1,y2;};
struct carousel_model {unsigned int (*art_version)(int);void (*draw_text)(void);void (*set_initial)(const char*);};
static const struct carousel_model provider_model;
static const struct carousel_model *model=&provider_model;
static int center_index,resume_index,previous_center;
static bool flicking,caption_valid;
static long moved_at;
long current_tick;
static unsigned int revision,seen_caption_version;
static int caption_index,caption_variant,draws,scroll_builds;
static bool caption_owed;
static char drawn[2][260],scroll[2][260];
static struct {int album_covers_show_album_name,album_covers_show_year;}global_settings={4,0};
static int pf_fg_color,pf_bold_font;
static struct viewport pf_vp;
static int pf_bg_color;
static int font(void){return 1;}
static struct {int(*getuifont)(void);}screens[]={{font}};
static int count(void){return cider_library_total();}
static void set_current_slide(int i){center_index=i;}
static bool cider_page_row(int i,struct cider_row*r){return cider_library_row(i,r);}
static int cider_page_total(void){return cider_library_total();}
static void cider_page_open(const char*s){cider_library_open(s);}
static unsigned int cider_art_revision(void){return 0;}
static bool cider_has_art(const char*k,const char*t){(void)k;(void)t;return false;}
void cider_browse_album(const struct cider_row*r){cider_library_open(r->route);}
static void cider_cancel_art(void){}
static struct viewport *carousel_text_begin(void){return NULL;}
static void carousel_text_end(struct viewport*v){(void)v;}
static void lcd_set_foreground(int i){(void)i;}
static void lcd_set_background(int i){(void)i;}
static void lcd_set_drawmode(int i){(void)i;}
static void lcd_setfont(int i){(void)i;}
static void lcd_set_viewport(struct viewport*v){(void)v;}
static const char*carousel_caption_fit(const char*s,char*b,size_t n){snprintf(b,n,"%s",s);return b;}
static void set_scroll_line(const char*s,int n){snprintf(scroll[n],sizeof scroll[n],"%s",s);scroll_builds++;}
static void carousel_caption_layout(bool two,struct pf_caption*c){c->y1=0;c->y2=two?1:0;}
static int get_scroll_line_offset(int n){(void)n;return 0;}
static void lcd_putsxy(int x,int y,const char*s){(void)x;assert(y>=0&&y<2);snprintf(drawn[y],sizeof drawn[y],"%s",s);draws++;}
static int agents_nav_pending(void){return AGENTS_NAV_NONE;}
static void splash(int n,const char*s){(void)n;(void)s;assert(0);}
static int quick_mode,overlay_calls,waits,joins,restarts;
static void deliver(int index,const char*title,const char*artist){
 struct cider_row row;assert(!cider_library_row(index,&row));
 char request[512],wire[1024];unsigned int gen,req;int offset;
 assert(cider_library_next(request,sizeof request,7)>0);
 assert(sscanf(request,"cq\t%u\t%u\t%d",&gen,&req,&offset)==3&&offset==index);
 int n=snprintf(wire,sizeof wire,"CL\t7\t%u\t1\t%d\t200\t0\t1\nid-%d\t%s\t%s\tart-%d\t0\t1\talbum\talbum/id-%d\t123456\t2026\n",req,index,index,title,artist,index,index);
 assert(n>0&&(size_t)n<sizeof wire);
 assert(cider_library_receive(wire,n,7,1));
}
static int get_action(int context,int timeout){
 (void)context;assert(timeout==HZ/10);waits++;current_tick+=timeout;
 deliver(150,"Other page","Other artist");return ACTION_NONE;
}
static int quick_screen_current(int button,void*arg){
 (void)button;(void)arg;overlay_calls++;
 if(quick_mode)deliver(center_index,"Delayed Album","Delayed Artist");
 return 0;
}
static void end_pf_thread(void){joins++;}
static bool create_pf_thread(void){restarts++;return true;}
static const char *model_title(void){return "Cider";}
static void sb_set_persistent_title(const char*s,int i,int n){(void)s;(void)i;(void)n;}
static int carousel_run(const struct carousel_model*m,const char*f);
'''
post = r'''
static int run_quick(void){
 int button=ACTION_STD_QUICKSCREEN,ret;
 switch(button){
QUICK_BRANCH
 }
 return 0;
}
static void ui_tick(void){
 current_tick++;
 if(update_caption_version(&seen_caption_version))caption_owed=true;
 if(caption_owed){model->draw_text();caption_owed=false;}
}
static int expected_index;
static bool before_draw,from_quick;
static int carousel_run(const struct carousel_model*m,const char*f){
 assert(m==model);m->set_initial(f);assert(center_index==expected_index);
 caption_valid=false;seen_caption_version=0;caption_owed=true;
 if(before_draw){
  if(from_quick){quick_mode=1;assert(run_quick()==0);quick_mode=0;}
  else deliver(center_index,"Delayed Album","Delayed Artist");
 }
 ui_tick();
 if(!before_draw){
  assert(!strcmp(drawn[0],"...")&&!strcmp(scroll[0],"..."));
  int old_builds=scroll_builds,old_draws=draws;unsigned old_revision=revision;
  if(from_quick){quick_mode=1;assert(run_quick()==0);quick_mode=0;}
  else deliver(center_index,"Delayed Album","Delayed Artist");
  long arrived=current_tick;
  ui_tick();
  assert(current_tick==arrived+1&&revision==old_revision&&center_index==expected_index);
  assert(draws==old_draws+2&&scroll_builds==old_builds+2);
 }
 assert(!strcmp(drawn[0],"Delayed Album")&&!strcmp(drawn[1],"Delayed Artist"));
 assert(!strcmp(scroll[0],"Delayed Album")&&!strcmp(scroll[1],"Delayed Artist"));
 int old_draws=draws,old_builds=scroll_builds;
 for(int i=0;i<20;i++)ui_tick();
 assert(draws==old_draws&&scroll_builds==old_builds); /* no polling redraw loop */
 return GO_TO_PREVIOUS;
}
int main(void){
 for(int restored=0;restored<2;restored++)for(int early=0;early<2;early++)for(int return_path=0;return_path<3;return_path++){
  cider_library_open("reset-fixture");cider_library_reset();
  resume_index=expected_index=restored?75:0;before_draw=early;from_quick=return_path==2;
  int old_waits=waits;
  assert(cider_covers()==GO_TO_PREVIOUS&&waits==old_waits+1);
  if(return_path==1){
   /* Real provider album callback changes route, then Cover Flow re-enters. */
   assert(enter(center_index)==GO_TO_PREVIOUS&&resume_index==expected_index);
   assert(cider_covers()==GO_TO_PREVIOUS&&center_index==expected_index);
  }
  printf("FU39 offset=%d page=%s return=%s PASS\n",expected_index,early?"before-first-draw":"after-first-draw",return_path==0?"entry":return_path==1?"album":"quick");
 }
 assert(joins>0&&joins==restarts&&overlay_calls==joins);
 /* Providers without version callbacks retain the existing local behavior. */
 const struct carousel_model local={0};model=&local;
 unsigned int seen=123;assert(!update_caption_version(&seen)&&seen==123);
 puts("FU39 real CL replies, provider entry/album return, quick overlay, caption rebuild within one UI tick; idle bounded PASS");
}
'''.replace('QUICK_BRANCH',quick)
(O/'config.h').write_text('')
(O/'kernel.h').write_text('#define HZ 100\nextern long current_tick;\n#define TIME_BEFORE(a,b) ((long)((a)-(b))<0)\n')
functions='\n'.join([fn(carousel,'carousel_caption_changed'),update,fn(carousel,'carousel_album_caption'),fn(provider,'initial'),fn(provider,'enter'),fn(provider,'text'),fn(provider,'version')])
model='static const struct carousel_model provider_model={version,text,initial};\n'
entry=fn(provider,'cider_covers').replace('&model,','&provider_model,')
(O/'test.c').write_text(pre+functions+model+entry+post)
subprocess.run(['cc','-std=gnu11','-Wall','-Wextra','-Werror','-g','-fsanitize=address,undefined','-fno-pie','-no-pie','-I'+str(O),'-I'+str(R/'apps-ipod'),str(O/'test.c'),str(R/'apps-ipod/screens/playback/cider_library.c'),'-o',str(O/'test')],check=True)
subprocess.run([str(O/'test')],check=True)
