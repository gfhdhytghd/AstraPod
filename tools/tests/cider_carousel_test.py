"""Real native slide adapter + Cider M/XS/bg provider, with bounded fake buflib."""
from pathlib import Path
import re,subprocess,sys
R=Path(__file__).resolve().parents[2];O=Path(sys.argv[1]).resolve();O.mkdir(parents=True,exist_ok=True)
def function(s,name):
 m=re.search(r'(?m)^(?:static\s+)?(?:inline\s+)?[\w *]+\b'+name+r'\([^;{}]*\)\s*\{',s);assert m,name
 e=m.end();n=1
 while n:n+=(s[e]=='{')-(s[e]=='}');e+=1
 return s[m.start():e]
carousel=(R/'apps-ipod/screens/covers/carousel.c').read_text();provider=(R/'apps-ipod/screens/covers/cider_covers.c').read_text();art_provider=Path(sys.argv[2]).read_text() if len(sys.argv)>2 else provider
pre=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "screens/playback/cider_library.h"
typedef uint16_t pix_t;typedef uint16_t fb_data;
struct bitmap{int width,height,format;unsigned char*data;};struct dim{int width,height;};
#define FORMAT_NATIVE 1
#define LCD_RGBPACK(r,g,b) (((r)&0xf8)<<8|((g)&0xfc)<<3|((b)>>3))
#define CIDER_COVER_DIM 128
#define CIDER_COVER_SCRATCH_BYTES (CIDER_COVER_DIM * CIDER_COVER_DIM * sizeof(uint16_t))
#define SLIDE_CACHE_SIZE 100
#define ALBUM_COVERS_NUM_SLIDES 4
#define PF_SHOW_STATS 0
#define MAX_PATH 260
#define O_RDONLY 0
static int center_index,pf_cover_size_idx=-1,empty_slide_hid=99,buf_ctx,pf_slides_loaded;
static struct {struct {int index,hid;unsigned int version;}cache[100];}pf_sldcache;
static void*handles[20];static unsigned int sizes[20];static int allocated;
static size_t remaining=3*(sizeof(int)+sizeof(struct dim)+32768+16384);
static int evictable,evictions;
static int buflib_alloc(int*c,unsigned int n){
 (void)c;if(n>remaining)return -1;
 remaining-=n;assert(allocated<19);int h=++allocated;handles[h]=calloc(1,n);sizes[h]=n;return h;
}
static bool free_slide_prio(int prio){
 assert(prio==0);
 if(!evictable)return false;
 int h=pf_sldcache.cache[evictable].hid;
 remaining+=sizes[h];free(handles[h]);handles[h]=NULL;
 pf_sldcache.cache[evictable].hid=0;evictable=0;evictions++;
 return true;
}
static void*buflib_get_data(int*c,int h){(void)c;assert(h>0&&h<=allocated);return handles[h];}
static void art_cache_thumb_path(unsigned int h,int i,char*p,int n){(void)h;(void)i;(void)p;(void)n;assert(0);}
static int open(const char*p,int f){(void)p;(void)f;assert(0);return -1;}
static void close(int f){(void)f;}
static int read_aat(int f,int p){(void)f;(void)p;assert(0);return -1;}
static void img_filter_apply_banded(void*p,int h,int w,const void*f){(void)p;(void)h;(void)w;(void)f;}
static const void*carousel_filter(void){return NULL;}
static struct {unsigned int(*art_key)(int);bool(*read_art)(int,struct bitmap*);unsigned int(*art_version)(int);} model_data,*model=&model_data;
static fb_data scratch_pixels[128*128],*surface=scratch_pixels;static int quality;
static int visited[40],nvisited;
static bool cider_page_row(int index,struct cider_row*r){if(nvisited<40)visited[nvisited++]=index;memset(r,0,sizeof*r);r->bg=0x123456;strcpy(r->art,"key");return index>=0;}
static bool cider_copy_art(const char*k,const char*t,void*p,unsigned int n){(void)k;if((!strcmp(t,"M")&&quality!=3)||(!strcmp(t,"XS")&&quality<2))return false;uint16_t*out=p;for(unsigned int i=0;i<n/2;i++)out[i]=i;return true;}
static bool local_store;
static bool cider_art_local(const char*k,const char*t){(void)k;(void)t;return local_store;}
static unsigned int key(int i){(void)i;return 0;}
static unsigned int version(int i){(void)i;return quality;}
'''
tick_stubs=r'''
#define HZ 100
#define ARRAYLEN(a) (sizeof(a)/sizeof((a)[0]))
static struct cider_row row_ahead;
static unsigned cider_read_ahead_count(unsigned s,unsigned v){(void)s;(void)v;return 6;}
static void cider_read_ahead(const char*const*k,unsigned n,const char*t){(void)k;assert(n<=15);assert(!strcmp(t,"M"));}
#define MIN(a,b) ((a)<(b)?(a):(b))
#define TIME_BEFORE(a,b) ((long)((a)-(b))<0)
static long current_tick,moved_at;
static int previous_center,travel=1;
static bool flicking;
static unsigned int revision;
static int m_requests,xs_requests;
static const char*retained_tier;
static int count(void){return 20;}
static bool cider_has_art(const char*k,const char*t){(void)k;return !strcmp(t,"M")?quality==3:quality>=2;}
static void cider_prefetch_art(const char*k,const char*t){(void)k;assert(!strcmp(t,"M"));m_requests++;}
static void cider_request_xs(const char*const*k,unsigned int n){(void)k;assert(n>0&&n<=5);xs_requests++;}
static void cider_warm_art(const char*const*k,unsigned int n,const char*t){(void)k;assert(n<=7);(void)t;}
static void cider_retain_art(const char*const*k,unsigned int n,const char*t){(void)k;assert(n==9);retained_tier=t;}
static unsigned int cider_art_revision(void){return 0;}
static void carousel_provider_changed(void){}
'''
caption_stubs=r'''
#define TEXT_FIT_BUF 80
#define ALBUM_NAME_HIDE 0
#define ALBUM_AND_ARTIST_TOP 3
#define ALBUM_AND_ARTIST_BOTTOM 4
#define ALBUM_YEAR_BEFORE 1
#define ALBUM_YEAR_AFTER 2
#define PF_SCROLL_ALBUM 0
#define PF_SCROLL_ARTIST 1
struct viewport {int dummy;};struct pf_caption {int y1,y2;};
static struct {int album_covers_show_album_name,album_covers_show_year;}global_settings;
static int pf_fg_color,pf_bold_font;
static int font(void){return 1;}
static struct {int(*getuifont)(void);}screens[]={{font}};
#define SCREEN_MAIN 0
static int drawn;static char text_lines[2][260];
static struct viewport*carousel_text_begin(void){return NULL;}
static void carousel_text_end(struct viewport*p){(void)p;}
static void lcd_set_foreground(int n){(void)n;}
static void lcd_setfont(int n){(void)n;}
static bool carousel_caption_changed(int i,int v){(void)i;(void)v;return true;}
static const char*carousel_caption_fit(const char*s,char*b,size_t n){snprintf(b,n,"%s",s);return b;}
static void set_scroll_line(const char*s,int n){(void)s;(void)n;}
static void carousel_caption_layout(bool two,struct pf_caption*c){c->y1=0;c->y2=two?1:0;}
static int get_scroll_line_offset(int n){(void)n;return 0;}
static void lcd_putsxy(int x,int y,const char*s){(void)x;(void)y;assert(drawn<2);snprintf(text_lines[drawn++],260,"%s",s);}
'''
lifetime_stubs=r'''
enum { GO_TO_PREVIOUS, GO_TO_ROOT, CAROUSEL_REMOTE_RETURN,
       ACTION_STD_QUICKSCREEN, ACTION_STD_CANCEL, ACTION_STD_MENU, CONTEXT_STD,
       QUICKSCREEN_IN_USB, AGENTS_NAV_NONE };
#define ID2P(x) #x
static bool room=true,alloc_fails,pinned,allocated_scratch;
static int opens,frees,failures,cancels,alloc_calls;
static void cider_page_open(const char*r){assert(!strcmp(r,"albums"));}
static int cider_page_total(void){return 5;}
static int get_action(int c,int t){(void)c;(void)t;assert(0);return 0;}
static int quick_screen_current(int a,void*p){(void)a;(void)p;assert(0);return 0;}
static int agents_nav_pending(void){return AGENTS_NAV_NONE;}
static void splash(int t,const char*s){(void)t;assert(!strcmp(s,"LANG_OUT_OF_MEMORY"));failures++;}
static size_t core_allocatable(void){return room?sizeof scratch_pixels:0;}
static int core_alloc(size_t bytes){
 assert(bytes==32768&&!allocated_scratch&&!pinned);
 alloc_calls++;if(alloc_fails)return -1;
 allocated_scratch=true;return 7;
}
static void*core_get_data_pinned(int h){assert(h==7&&allocated_scratch&&!pinned);pinned=true;return scratch_pixels;}
static int carousel_run(const void*m,const char*f){
 (void)m;(void)f;assert(pinned&&surface==scratch_pixels);opens++;return CAROUSEL_REMOTE_RETURN;
}
static void core_unpin(int h){assert(h==7&&allocated_scratch&&pinned&&!surface);pinned=false;}
static int core_free(int h){assert(h==7&&allocated_scratch&&!pinned&&!surface);allocated_scratch=false;frees++;return 0;}
static void cider_cancel_art(void){assert(!allocated_scratch);cancels++;}
'''
assert 'carousel_album_caption(' in function(provider,'text')
assert 'lcd_' not in function(provider,'text')
assert 'carousel_album_caption(' in function((R/'apps-ipod/screens/covers/album_covers.c').read_text(),'draw_album_text')
post=r''' 
int main(void){model_data.art_key=key;model_data.read_art=art;model_data.art_version=version;
assert(!load_and_prepare_surface(0,-1,0));assert(!load_and_prepare_surface(0,100,0));
for(int q=1;q<=3;q++){quality=q;assert(load_and_prepare_surface(0,q,0));int h=pf_sldcache.cache[q].hid;struct dim*d=get_slide(h);assert(d->width==128&&d->height==d->width);uint16_t*p=(uint16_t*)(d+1);
if(q==3){assert(p[129]==129&&sizes[h]==sizeof(int)+sizeof(struct dim)+32768+16384);}
else if(q==2){for(int x=0;x<128;x++)for(int y=0;y<128;y++)assert(p[x*128+y]==(y/4)*32+x/4);}
else assert(p[0]==LCD_RGBPACK(0x12,0x34,0x56));
assert(pf_sldcache.cache[q].version==(unsigned)q);}
assert(!remaining&&!load_and_prepare_surface(0,4,0)&&!pf_sldcache.cache[4].hid);
evictable=3;
assert(load_and_prepare_surface(0,4,0)&&evictions==1&&!pf_sldcache.cache[3].hid);
assert(pf_sldcache.cache[1].hid&&pf_sldcache.cache[2].hid);
int relocated=pf_sldcache.cache[4].hid;
void *moved=malloc(sizes[relocated]);assert(moved);
memcpy(moved,handles[relocated],sizes[relocated]);
free(handles[relocated]);handles[relocated]=moved;
struct dim*d=get_slide(relocated);
uint16_t*p=(uint16_t*)(d+1);
assert(p[129]==129&&p[128*128]==64&&!remaining);
for(int i=1;i<=allocated;i++){free(handles[i]);}
for(int show=0;show<=4;show++){for(int year=0;year<3;year++){
 global_settings.album_covers_show_album_name=show;global_settings.album_covers_show_year=year;drawn=0;
 carousel_album_caption("Album","Artist",2020,false,0);
 assert(drawn==(show==0?0:show==3||show==4?2:1));
 if(drawn){assert(strstr(text_lines[0],"Album"));if(year)assert(strstr(text_lines[0],"2020"));if(drawn==2)assert(!strcmp(text_lines[1],"Artist"));}
}}
quality=1;center_index=6;previous_center=5;current_tick=100;moved_at=0;nvisited=0;tick();assert(!flicking&&m_requests==1&&xs_requests==1&&!strcmp(retained_tier,"M"));
center_index=5;current_tick=110;nvisited=0;tick();assert(flicking&&xs_requests==2&&visited[0]==5&&visited[1]==4&&!strcmp(retained_tier,"M"));
current_tick=145;tick();assert(!flicking&&m_requests==3&&!strcmp(retained_tier,"M"));
local_store=true;quality=2;struct bitmap pending;assert(!art(0,&pending));local_store=false;
surface=NULL;
for(int visit=0;visit<2;visit++)assert(cider_covers()==CIDER_COVERS_REMOTE_RETURN);
assert(opens==2&&frees==2&&cancels==2&&!surface&&!allocated_scratch&&alloc_calls==2);
room=false;assert(cider_covers()==GO_TO_PREVIOUS&&failures==1&&alloc_calls==2);
room=true;alloc_fails=true;assert(cider_covers()==GO_TO_PREVIOUS&&failures==2&&alloc_calls==3);
assert(opens==2&&frees==2&&!surface);
puts("Native carousel load/get_slide layout + real Cider M columns, XS transpose, bg placeholder PASS");}
'''
(O/'carousel.c').write_text(pre+function(carousel,'side_level_bytes')+function(carousel,'build_side_level')+function(art_provider,'art')+'\n'+function(carousel,'load_and_prepare_surface')+'\n'+function(carousel,'get_slide')+tick_stubs+function((R/'apps-ipod/screens/playback/cider_model.c').read_text(),'cider_window')+function(provider,'tick')+caption_stubs+function(carousel,'carousel_album_caption')+lifetime_stubs+function(provider,'cider_covers')+post)
subprocess.run(['cc','-std=gnu11','-Wall','-Wextra','-Werror','-g','-fsanitize=address,undefined','-fno-pie','-no-pie','-I'+str(R/'apps-ipod'),str(O/'carousel.c'),'-o',str(O/'carousel')],check=True);subprocess.run([str(O/'carousel')],check=True)
