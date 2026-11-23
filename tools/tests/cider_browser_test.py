"""Production native menu loop with instrumented widgets/provider/navigation."""
from pathlib import Path
import re,subprocess,sys
R=Path(__file__).resolve().parents[2];O=Path(sys.argv[1]).resolve();O.mkdir(parents=True,exist_ok=True)
s=(R/'apps-ipod/screens/playback/cider_browser.c').read_text();s=re.sub(r'^#include .*\n','',s,flags=re.M)
tokens=sorted(set(re.findall(r'\b(?:ACTION_|Icon_|LANG_|CONTEXT_|ACTIVITY_|GO_TO_)[A-Z_a-z0-9]+',s)))
pre=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "screens/playback/cider_library.h"
#include "screens/playback/cider_model.h"
#define ARRAYLEN(x) (sizeof(x)/sizeof((x)[0]))
#define HZ 100
#define TIME_BEFORE(a,b) ((long)((a)-(b))<0)
#define MAX(a,b) ((a)>(b)?(a):(b))
#define MIN(a,b) ((a)<(b)?(a):(b))
#define SEARCH_MAX_QUERY 32
#define P2STR(s) ((const char*)(s))
#define FORMAT_NATIVE 1
#define SYS_USB_CONNECTED 0xffff
#define QUICKSCREEN_IN_USB 1
static int quick_screen_current(int action,void(*poll)(void)){(void)action;(void)poll;return 0;}
'''
tokens=sorted((set(tokens)|{'ACTION_STD_MENU','ACTION_REDRAW'})-{'ACTION_NONE'})
pre+='enum {ACTION_NONE,'+','.join(tokens)+'};\n'
pre+=r'''
enum themable_icons {icon_dummy=1000};
enum list_row_kind {LIST_ROW_PLAIN,LIST_ROW_TRACK,LIST_ROW_CONTAINER};
enum agents_nav_screen {AGENTS_NAV_NONE,AGENTS_NAV_TRACKS,AGENTS_NAV_WPS,AGENTS_NAV_AGENTS,AGENTS_NAV_COVERS,AGENTS_NAV_BACK};
typedef uint16_t fb_data;
struct dim {int width,height;};
struct bitmap{int width,height,format;unsigned char*data;};
struct browser_db_header {char name[128],artist[128];int tracks,year;long length_ms;};
struct list_header {
    const char *title, *subtitle, *detail;
    const struct bitmap *(*cover)(void *, struct dim *);
    void *data;
};
struct gui_synclist{int selected,count;const struct list_header*header;void (*callback_prefetch_art)(int,void*,int);};
struct artist_tile_source { unsigned identity; char remote[65]; };
static unsigned art_name_hash(const char *name){(void)name;return 1;}
static const struct bitmap *artist_tile_request(const struct artist_tile_source *src,int dim,bool paint)
{(void)src;(void)dim;(void)paint;assert(!"unexpected artist art");return NULL;}
struct ui_scene { int handle; bool capturing; };
static long current_tick;
static void ui_scene_interrupt(void){}
static void ui_scene_cancel(struct ui_scene *s){assert(!s->handle);}
static void ui_scene_start(struct ui_scene *s){assert(!s->handle);}
static void ui_scene_update(struct ui_scene *s,bool redraw){(void)redraw;assert(!s->handle);}
static int ui_scene_timeout(struct ui_scene *s,int timeout){assert(!s->handle);return timeout;}
static bool ui_scene_capture(struct ui_scene *s,const void *a,const void *b,bool push)
{(void)a;(void)b;(void)push;assert(!s->handle);return false;}
static void browser_album_details(const struct browser_db_header*h,char*detail,size_t n)
{snprintf(detail,n,"%d tracks",h->tracks);}
static void *view_storage;
static bool view_pinned;
static int core_alloc_free_space(size_t n)
{
    assert(!view_storage);
    view_storage = calloc(1, n);
    assert(view_storage);
    return 1;
}
static void *core_get_data_pinned(int h)
{
    assert(h == 1 && view_storage && !view_pinned);
    view_pinned = true;
    return view_storage;
}
static void core_put_data_pinned(void *data)
{
    assert(data == view_storage && view_pinned);
    view_pinned = false;
}
static int core_free(int h)
{
    assert(h == 1 && view_storage && !view_pinned);
    free(view_storage);
    view_storage = NULL;
    return 0;
}
struct search_provider{const char*title;int activity;int(*scan)(const char*,void*);const char*(*row_text)(int,void*);bool(*refresh)(void*);};
static struct {bool db_albumart;int db_art_row_height;bool db_artistart;}global_settings={true,46,false};
static bool origin_agents,journey,origin_wps,cover_music;
static int cover_origin;
static bool cover_nav_active(void){return journey;}
static void cover_nav_begin(int s){if(!journey){cover_music=false;cover_origin=s;origin_wps=s==GO_TO_WPS;journey=true;}}
static int cover_nav_finish(void){journey=cover_music=false;return origin_agents?GO_TO_AGENTS:cover_origin;}
static bool cover_nav_playing(void){return journey&&cover_music;}
static void cover_nav_set_playing(bool playing){cover_music=playing;}
static void cider_remote_source(void){}
static bool cover_nav_origin_is(int s){return (origin_agents&&s==GO_TO_AGENTS)||(origin_wps&&s==GO_TO_WPS);}
static bool fail_play;
static int cider_play_status(void){return 1;}
static void splashf(int t,const char*f,...){(void)t;(void)f;}
__attribute__((unused)) static void splash(int t,const char*s){(void)t;(void)s;}
static const char*cider_remote_name(void){return "Cider";}
static int remote_homes;static void cider_remote_home(void){remote_homes++;}
static bool cider_store_debug_show(void){return false;}
static unsigned cider_read_ahead_count(unsigned s,unsigned v){(void)s;(void)v;return 6;}
static void cider_read_ahead(const char*const*k,unsigned n,const char*t){(void)k;(void)n;(void)t;}
static void cider_warm_art(const char*const*k,unsigned int n,const char*t){(void)k;(void)n;(void)t;}
static bool available=true;static int live,max_live,plays,draws,step,mode;static uint32_t rng;
static char route[380];
static const char*str(int n){(void)n;return "Native";}
static size_t test_strlcpy(char *out, const char *in, size_t n)
{
    return snprintf(out, n, "%s", in);
}
#define strlcpy test_strlcpy
static bool cider_connected(void){return available;}
static bool cider_available(void){return available;}
static bool menu_back;static bool action_menu_back(void){return menu_back;}
static void carousel_forget_resume(void){}
static bool cider_active(void){return false;}
static int audio_status(void){return 1;}
static bool agents_nav_enter(int n){(void)n;assert(live==0);live++;return true;}
static void agents_nav_leave(int n){(void)n;assert(live==1);live--;}
static void agents_nav_take(int n){(void)n;}
static int agents_nav_pending(void){return AGENTS_NAV_NONE;}
static void push_current_activity(int n){(void)n;}
static void push_activity_without_refresh(int n){push_current_activity(n);}
static void pop_current_activity(void){}
static int browser_db_root_row_count(void){return 3;}
static bool browser_db_get_root_row(int n,int*index,const unsigned char**name){static const char*names[]={"Album","Artist","Genre"};*index=n;if(name)*name=(const unsigned char*)names[n];return true;}
static bool browser_db_root_row_is_hidden(int index){return index==1;}
static const char*browser_db_root_row_key(int index){static const char*names[]={"Album","Artist","Genre"};return names[index];}
static void cider_page_open(const char*p){snprintf(route,sizeof route,"%s",p);}
static int cider_page_total(void)
{
    if (mode == 5 && (step == 1 || step == 2)) return -1;
    return 3;
}
static bool cider_page_stale(void){return false;}
static bool cider_page_row(int i,struct cider_row*r){if(i<0||i>=3)return false;memset(r,0,sizeof*r);snprintf(r->title,sizeof r->title,"Album %d",i);if(!strncmp(route,"album/",6)||!strcmp(route,"random")){r->kind=CIDER_ROW_SONG;snprintf(r->route,sizeof r->route,"play/library-songs/s%d",i);}else{r->kind=CIDER_ROW_ALBUM;memset(r->art,'c',64);r->count=9;r->year=2026;strcpy(r->subtitle,"Artist");snprintf(r->route,sizeof r->route,"album/a%d",i);}return true;}
static void cider_cancel_art(void){}
static int pins;static bool pinned;
static void cider_pin_art(const char*k){assert(strlen(k)==64);pins++;pinned=true;}
static void cider_unpin_art(void){pinned=false;}
static int fixture_art;
static bool cider_copy_art(const char*k,const char*t,void*p,unsigned int n)
{
    (void)k;
    if (!fixture_art || fixture_art != (!strcmp(t, "XS") ? 32 : 56)) return false;
    assert(n == fixture_art * fixture_art * sizeof(fb_data));
    for (unsigned i = 0; i < n / sizeof(fb_data); i++) ((fb_data *)p)[i] = i;
    return true;
}
static bool cider_prefetch_art(const char*k,const char*t){(void)k;(void)t;return false;}
static bool cider_art_local(const char*k,const char*t){(void)k;(void)t;return false;}
static bool cider_has_art(const char*k,const char*t){(void)k;(void)t;return false;}
static void cider_request_xs(const char*const*k,unsigned int n){(void)k;(void)n;}
static bool cider_play(const char*p){assert(available&&live==1&&strstr(p,"play/"));if(fail_play)return false;plays++;return true;}
static char last_ctx[400];static int last_idx=-1;static bool cider_play_in(const char*p,const char*c,int i){snprintf(last_ctx,sizeof last_ctx,"%s",c);last_idx=i;return cider_play(p);}
int cider_covers(void);
static int default_event_handler(int a){(void)a;return 0;}
static int search_dialog_run(const struct search_provider*p,void*c,char*q,size_t n){(void)n;p->scan(q,c);p->refresh(c);p->row_text(0,c);return -1;}
static int dialog_input(char*p,int n){snprintf(p,n,"text");return 0;}
static void gui_synclist_init(struct gui_synclist*l,const char*(*n)(int,void*,char*,size_t),void*d,bool s,int c,void*v){(void)n;(void)d;(void)s;(void)c;(void)v;memset(l,0,sizeof*l);}
static void gui_synclist_set_icon_callback(struct gui_synclist*l,enum themable_icons(*f)(int,void*)){(void)l;(void)f;}
static void gui_synclist_set_row_callbacks(struct gui_synclist*l,enum list_row_kind(*f)(int,void*),void*a,void*b){(void)l;(void)f;(void)a;(void)b;}
static void gui_synclist_set_albumart_callback(struct gui_synclist*l,const struct bitmap*(*f)(int,void*,struct dim*)){(void)l;(void)f;}
static void gui_synclist_set_row_height(struct gui_synclist*l,int h){(void)l;(void)h;}
static void gui_synclist_set_title(struct gui_synclist*l,const char*t,int i){(void)l;(void)t;(void)i;}
static void gui_synclist_set_header(struct gui_synclist*l,const struct list_header*h){l->header=h;}
static void gui_synclist_set_nb_items(struct gui_synclist*l,int n){l->count=n;}
static void gui_synclist_select_item(struct gui_synclist*l,int i){l->selected=MAX(0,MIN(i,l->count-1));}
static int gui_synclist_get_sel_pos(struct gui_synclist*l){return l->selected;}
static int header_draws;
static void gui_synclist_draw(struct gui_synclist*l)
{
    draws++;
    if (l->header)
    {
        assert(view_pinned && l->header->cover && l->header->data == view_storage);
        assert(*l->header->detail);
        header_draws++;
    }
}
static void gui_synclist_draw_settled(struct gui_synclist *list){gui_synclist_draw(list);}
static void gui_synclist_scroll_stop(struct gui_synclist*l){(void)l;}
static void gui_synclist_forget(struct gui_synclist*l){l->header=NULL;}
static bool list_do_action(int context,int timeout,struct gui_synclist*l,int*a){(void)context;(void)timeout;step++;
if(mode==5){
    if(step==1){assert(l->selected==2);*a=ACTION_STD_OK;}
    else if(step==2){
        assert(l->header&&!strcmp(l->header->title,"Album 2")&&l->count==1);
        assert(!strcmp(l->header->subtitle,"Artist")&&!strcmp(l->header->detail,"9 tracks"));
        assert(l->header->cover && pinned);
        *a=ACTION_STD_CANCEL;
    } else if(step==3){
        assert(l->selected==0&&!l->header&&l->count==1);*a=ACTION_REDRAW;
    } else if(step==4){
        assert(l->selected==2&&!l->header&&l->count==3);*a=ACTION_STD_MENU;
    } else {
        *a=ACTION_STD_MENU;
    }
    return false;
}
if(mode==4){*a=ACTION_STD_MENU;return false;}if(mode==3){*a=step==1?ACTION_STD_OK:ACTION_STD_CANCEL;return false;}if(mode==0){*a=ACTION_STD_OK;return false;}if(mode==1){*a=ACTION_STD_MENU;return false;}
if(step>128){*a=ACTION_REMOTE_WPS;return false;}rng=rng*1664525+1013904223;l->selected=(rng>>16)%MAX(1,l->count);*a=(rng&3)==0?ACTION_STD_CANCEL:(rng&3)==1?ACTION_STD_OK:ACTION_NONE;return false;}
'''
post=r'''
int cider_covers(void){if(mode==4)return CIDER_COVERS_REMOTE_RETURN;if(mode==1)return GO_TO_PREVIOUS;assert(live==1);live++;if(live>max_live)max_live=live;live--;return GO_TO_PREVIOUS;}
static void check_art_pixels(void)
{
    struct cider_browser_view view;
    for (int source = 32; source <= 56; source += 24) {
      fixture_art = source;
      for (int dim = 1; dim <= 56; dim++) {
        struct dim request = {dim, 60};
        const struct bitmap *artwork = bitmap_for(&view, "key", &request);
        assert(artwork && artwork->width == dim && artwork->height == dim);
        const fb_data *pixels = (const fb_data *)artwork->data;
        for (int y = 0; y < dim; y++)
            for (int x = 0; x < dim; x++)
                assert(pixels[y * dim + x] == y * source / dim * source + x * source / dim);
      }
    }
    fixture_art = false;
    puts("PASS Cider list art: pixel reference matches at all 56 output sizes");
}
int main(void){
check_art_pixels();
mode=0;depth=0;
struct cider_row album={.kind=CIDER_ROW_ALBUM};strcpy(album.route,"album/a0");strcpy(album.title,"Album 0");
memset(album.art,'c',64);
assert(cider_browse_at(&album)==GO_TO_WPS&&plays==1&&!live&&depth==0);
assert(root_count==6&&remote_homes==0);
struct cider_row root_row;assert(get_root_row(3,&root_row)&&!strcmp(root_row.route,"root/Album"));assert(get_root_row(4,&root_row)&&!strcmp(root_row.route,"root/Genre"));
mode=1;assert(cider_browse()==GO_TO_ROOT&&!live&&depth==0&&remote_homes==1);
depth=1;strcpy(history[1].route,"random");assert(cider_browse()==GO_TO_WPS&&depth==0&&!live);assert(cider_browse()==GO_TO_ROOT&&!live);
fail_play=true;depth=1;strcpy(history[1].route,"album/a0");memset(history[1].art,'c',64);history[1].art[64]=0;pins=0;mode=3;step=0;int raced=plays;assert(cider_browse()==GO_TO_ROOT&&plays==raced&&!live);assert(pins>0&&!pinned);fail_play=false;
depth=1;strcpy(history[1].route,"root/Album");history[1].header.name[0]=0;
history[1].selected=2;mode=5;step=0;assert(cider_browse()==GO_TO_ROOT&&step==5);
available=false;mode=2;step=0;int before=plays;cider_browse();assert(plays==before&&!live&&empty_list);available=true;
for(int i=1;i<=2000;i++){rng=i;step=0;mode=2;cider_browse();assert(!live);}
depth=0;mode=4;cider_browse_covers(true);assert(cider_browse()==GO_TO_WPS&&cider_browse_remote_return()&&!cider_browse_remote_return());
mode=1;cider_browse_covers(true);assert(cider_browse()==GO_TO_PREVIOUS&&!cider_browse_remote_return());
mode=4;cider_browse_covers(false);assert(cider_browse()==GO_TO_WPS&&cider_browse_remote_return());
origin_agents=true;cider_browse_covers(false);assert(cider_browse()==GO_TO_AGENTS&&cider_browse_remote_return());mode=1;cider_browse_covers(false);assert(cider_browse()==GO_TO_AGENTS);origin_agents=false;
mode=1;step=0;menu_back=true;struct cider_row entry={0};strcpy(entry.route,"album/context");assert(cider_browse_at(&entry)==GO_TO_PREVIOUS&&step==1);menu_back=false;
assert(max_live==2&&draws>2000&&header_draws>0&&!view_storage);printf("Production Cider native list controller: hidden rows, album summary lifetime, empty/unavailable, album/track play, 2000 seeded sequences, max instances=%d PASS\n",max_live);}
'''
model=(R/'apps-ipod/screens/playback/cider_model.c').read_text()
start=model.index('const char *cider_list_notice(');end=model.index('/* Compare only song routes;',start)
(O/'browser.c').write_text(pre+model[start:end]+s+post)
subprocess.run(['cc','-std=gnu11','-Wall','-Wextra','-Werror','-Wno-enum-conversion','-g','-fsanitize=address,undefined','-fno-pie','-no-pie','-I'+str(R/'apps-ipod'),str(O/'browser.c'),'-o',str(O/'browser')],check=True)
subprocess.run([str(O/'browser')],check=True,timeout=30)
