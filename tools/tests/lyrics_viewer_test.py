"""Execute the production viewer and document with recording UI/source adapters.

The simulator supplies visual proof. This fixture checks local/remote dispatch,
page-cache paging, input during retargets, idle return and arena-denied cleanup.
"""
from pathlib import Path
import ast,re,subprocess,sys
r=Path(__file__).resolve().parents[2];o=Path(sys.argv[1]).resolve();o.mkdir(parents=True,exist_ok=True)
ns=ast.parse((r/'tools/tests/lyrics_document_test.py').read_text())
pre=next(ast.literal_eval(n.value) for n in ns.body if isinstance(n,ast.Assign) and any(isinstance(t,ast.Name) and t.id=='pre' for t in n.targets))
pre=pre.replace('static struct {int default_codepage;} global_settings;', '''static struct {int default_codepage,lyric_colour_mode,backlight_timeout,backlight_timeout_plugged,lyric_prev_opacity,lyric_next_opacity,lyric_lead,lyric_align;unsigned fg_color,bg_color;bool lyric_highlight,lyric_backlight,prevent_skip,animations,native_ui;} global_settings;''')
pre=pre.replace('length,id3v2len;', 'length,id3v2len,elapsed;')
pre=pre.replace('static void core_free(int h){assert(h==1);frees++;}',
                'static int core_free(int handle);')
strutil=(r/'apps-ipod/system/strutil.c').read_text()
read_line=re.search(r'^int read_line\(.*?^}',strutil,re.M|re.S).group()
pre=re.sub(r'^static int read_line\([^\n]+\n', lambda _: '#include <errno.h>\n'+read_line+'\n', pre, flags=re.M)
source=(r/'apps-ipod/viewers/lyric_viewer/lyric_viewer.c').read_text()
source=re.sub(r'^#include ".*\n','',source,flags=re.M)
retained=(r/'apps-ipod/ui/retained.c').read_text()
retained=re.sub(r'^#include ".*\n','',retained,flags=re.M)
doc=(r/'apps-ipod/viewers/lyric_viewer/lyrics.c').read_text();doc=re.sub(r'^#include ".*\n','',doc,flags=re.M)
h=(r/'apps-ipod/viewers/lyric_viewer/lyrics.h').read_text();h=re.sub(r'^#include "metadata.h".*\n','',h,flags=re.M)
tokens=sorted(set(re.findall(r'\bACTION_[A-Z_0-9]+',source+' ACTION_WPS_BROWSE ACTION_UNKNOWN')))
button_codes=(r/'firmware/export/button.h').read_text()
button_codes='\n'.join(re.findall(r'^#define BUTTON_(?:MULTIMEDIA(?:_(?:PLAYPAUSE|NEXT|PREV|VOLUME_UP|VOLUME_DOWN))?|REPEAT)\s+[^\n]+',button_codes,re.M))
adapter=r'''
#define HAVE_USB_PODLINK
#define HAVE_ADJUSTABLE_CPU_FREQ
#define HAVE_MULTIMEDIA_KEYS
static unsigned active_trace;
static unsigned ui_perf_begin(const char *route)
{
    assert(!strncmp(route, "lyrics-", 7) && !active_trace);
    return active_trace = 1;
}
static void ui_perf_end(unsigned token)
{
    if (token)
    {
        assert(token == active_trace);
        active_trace = 0;
    }
}
static void check_trace_closed(void)
{
    assert(!active_trace);
}
static void ui_note_render(uint32_t elapsed) { (void)elapsed; }
static void ui_note_flush(uint32_t elapsed) { (void)elapsed; }
static bool shared_scene;
static bool lcd_updates_held(void) { return shared_scene; }
static bool lcd_active(void) { return true; }
static bool ui_scene_automatic(void) { return shared_scene; }
/* An independent playback owner remains throughout these visits. */
static int clock_refs = 1, boost_gets, boost_puts;
#define FREQ (clock_refs ? 216000000 : 54000000)
static int get_cpu_boost_counter(void) { return clock_refs; }
static void cpu_boost(bool on) {
    if (on) { assert(clock_refs == 1); boost_gets++; clock_refs++; }
    else { assert(clock_refs == 2); boost_puts++; clock_refs--; }
}
enum { AGENTS_NAV_AGENTS, AGENTS_NAV_COVERS };
static int nav_requests, nav_screen, nav_action;
static bool nav_remote;
static void agents_nav_request(int screen, bool remote)
{ nav_requests++; nav_screen = screen; nav_remote = remote; }
#define HZ 100
#define LCD_WIDTH 320
#define LCD_HEIGHT 240
#define LCD_WHITE 65535
#define LCD_BLACK 0
#define LCD_RGBPACK(r,g,b) ((((r)>>3)<<11)|(((g)>>2)<<5)|((b)>>3))
#define DRMODE_FG 1
#define BUTTON_REMOTE 0x8000
#define BUTTON_SELECT 1
#define BUTTON_RC_SELECT 0x00400000
#define FONT_DIR "fonts"
#define FONT_SYSFIXED 12
#define ROCKBOX_DIR "."
#include "lyric_viewer.h"
#include "ui/animation.h"
#include "ui/native_theme.h"
#define fdprintf dprintf
#define USEC_TIMER mock_usec()
#define TIMEOUT_NOBLOCK 0
#define MIN(a,b) ((a)<(b)?(a):(b))
#define MAX(a,b) ((a)>(b)?(a):(b))
#define TIME_BEFORE(a,b) ((int32_t)((uint32_t)(a)-(uint32_t)(b))<0)
#define IS_SYSEVENT(x) ((x)==SYS_USB_CONNECTED)
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
RETAINED_TEST_HEADER
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
static struct frame_buffer_t lcd_framebuffer_default={.fb_ptr=screen_pixels,.stride=320,.elems=320*240};
static struct mp3entry playing;
static struct wps_state wps={.id3=&playing};
static struct font {int height;} f12={12},f18={18},f26={26};
static int calls,plan,loads,unloads,claims,releases,holds,activities,updates,pauses,seeks,skips,controls,remote_at,default_events;
static size_t arena_bytes=sizeof app_area;
static fb_data panel[320*240];
static fb_data entry_core[2*320*240];
static bool deny_entry_core, entry_allocated, entry_pinned;
static int entry_allocs, entry_frees;
static int core_alloc_free_space(size_t size)
{
    assert(size==sizeof entry_core && !entry_allocated);
    if (deny_entry_core) return -1;
    entry_allocated=true;
    entry_allocs++;
    return 2;
}
static void *core_get_data_pinned(int handle)
{
    assert(handle==2 && entry_allocated && !entry_pinned);
    entry_pinned=true;
    return entry_core;
}
static void core_put_data_pinned(void *data)
{
    assert(data==entry_core && entry_pinned);
    entry_pinned=false;
}
static int core_free(int handle)
{
    if (handle==1)
    {
        frees++;
        return 0;
    }
    assert(handle==2 && entry_allocated && !entry_pinned);
    entry_allocated=false;
    entry_frees++;
    return 0;
}
static bool remote_source,deny_arena,inhibited,remote_key,playing_audio=true,page_missed;
static bool timed_fixture, wait_row, change_during_copy;
static struct cider_state remote_clock;
static int remote_opens, paused_width;
static int retarget_position, idle_updates, remote_actions, lead_boundary;
static int multimedia_count, multimedia_last, line_seeks, title_draws, manual_draws;
static long seek_target;
static unsigned seek_serial;
static int select_button, seek_updates, fast_seeks, no_lyrics_draws, nothing_playing_draws;
static bool verify_early_frame, reject_loading, backward_seek;
static int ui_font(void){return 18;}static struct screen screens[]={{ui_font}};
static struct font *font_get(int f){return f==12?&f12:f==26?&f26:&f18;}
static int font_load(const char *path){
    assert(clock_refs==2);
    if (verify_early_frame && !loads) {
        assert((updates || shared_scene) && "background must reach LCD before disk I/O");
        precise_extra+=1000000;
    }
    loads++;
    return strstr(path,"12-")?12:strstr(path,"26-")?26:18;
}
static void font_unload(int f){(void)f;unloads++;}
static unsigned width_reads;
static int font_get_width(struct font*f,unsigned ch){width_reads++;return ch>127?f->height:f->height/2;}
static const unsigned char *utf8decode(const unsigned char*p,ucschar_t *ch){*ch=*p++;if(*ch>=0xc0){int n=*ch>=0xf0?3:*ch>=0xe0?2:1;while(n--)*ch=(*ch<<6)|(*p++&63);}return p;}
static int font_getstringsize(const unsigned char*p,int*w,int*h,int f){*w=0;*h=f;while(*p){ucschar_t ch;p=utf8decode(p,&ch);*w+=font_get_width(font_get(f),ch);}return *w;}
static const char *text_wrap_next_punct(const char *p,int width,struct font*f,const char **end){int n=0;const char*s=p;while(*p&&*p!='\n'){ucschar_t ch;const char*q=(const char*)utf8decode((const unsigned char*)p,&ch);n+=font_get_width(f,ch);if(n>width&&p>s)break;p=q;}*end=p;return *p=='\n'?p+1:p;}
static unsigned color_blend(unsigned a,unsigned b,int p){unsigned rb=(((a&0xf81f)*(256-p)+(b&0xf81f)*p)>>8)&0xf81f;return rb|((((a&0x7e0)*(256-p)+(b&0x7e0)*p)>>8)&0x7e0);}
static struct viewport *lcd_set_viewport(struct viewport*v){struct viewport *old=vp;vp=v?v:&default_vp;return old;}
static void lcd_setfont(int f){vp->font=f;}
static void lcd_set_drawmode(int m){(void)m;}
static void lcd_set_foreground(unsigned c){vp->fg=c;}
static void lcd_fillrect(int x,int y,int w,int h){for(int r=MAX(y,0);r<MIN(y+h,240);r++)for(int c=MAX(x,0);c<MIN(x+w,320);c++)(vp->buffer?vp->buffer->fb_ptr:screen_pixels)[r*320+c]=vp->fg;}
static void lcd_putsxy(int x,int y,const char*p){
    if (reject_loading) assert(!strstr(p,"Loading") && "loading text must never be rasterized");
    if (!strcmp(p,"No lyrics")) no_lyrics_draws++;
    if (!strcmp(p,"Nothing playing")) nothing_playing_draws++;
    if (strstr(p,"Title")) title_draws++;
    if (!strcmp(p,"Manual")) manual_draws++;
    int w,h;font_getstringsize((const unsigned char*)p,&w,&h,vp->font);lcd_fillrect(x,y,w,h);
}
static void fill_round_rect(struct screen*s,int x,int y,int w,int h,int radius){(void)s;(void)radius;lcd_fillrect(x,y,w,h);}
static void memset16(void *ptr, int value, size_t count)
{
    fb_data *pixels = ptr;
    for (size_t i = 0; i < count; i++) pixels[i] = value;
}
static void lcd_update(void){updates++;memcpy(panel,screen_pixels,sizeof panel);}
static void lcd_update_rect(int x,int y,int w,int h){assert(x>=0&&y>=0&&w>0&&h>0&&x+w<=320&&y+h<=240);updates++;
 for(int j=y;j<y+h;j++)memcpy(panel+j*320+x,screen_pixels+j*320+x,w*sizeof(fb_data));}
static void sleep(int n){assert(clock_refs==2&&n>=0);current_tick+=n+1;}
static void *app_try_claim_buffer(size_t*n,const char*s){assert(!strcmp(s,"lyrics"));if(deny_arena)return NULL;*n=arena_bytes;claims++;return app_area;}
static void app_release_buffer(const char*s){assert(clock_refs==1);assert(!strcmp(s,"lyrics"));releases++;}
static bool ui_hold_scrolling(bool h){holds+=h?1:-1;assert(holds>=0);return !holds;}
static bool ui_flush_inhibited(void){return inhibited;}
static void ui_inhibit_flush(bool v){inhibited=v;}
static int dirty_rect[4];
static void ui_mark_dirty_rect(int screen,int x,int y,int w,int h)
{
    assert(!screen);
    dirty_rect[0]=x;dirty_rect[1]=y;dirty_rect[2]=w;dirty_rect[3]=h;
}
static void ui_flush_dirty(void)
{
    if (inhibited || !dirty_rect[2])
        return;
    lcd_update_rect(dirty_rect[0],dirty_rect[1],dirty_rect[2],dirty_rect[3]);
    dirty_rect[2]=0;
}
static unsigned art_generation;
static unsigned album_art_generation(void){return art_generation;}
static void push_current_activity(int a){(void)a;activities++;}
static void pop_current_activity(void){activities--;}
static void viewportmanager_theme_enable(int s,bool b,struct viewport*v){(void)s;(void)b;*v=(struct viewport){0};}
static void viewportmanager_theme_undo(int s,bool b){(void)s;(void)b;}
static void ui_theme_snapshot(struct ui_theme *theme)
{
    memset(theme, 0, sizeof(*theme));
    theme->background[UI_BG_LYRICS] = UI_BG_BLURRED_COVER;
    theme->color[UI_COLOR_FOREGROUND] = theme->color[UI_COLOR_ACCENT] = 0xffffff;
}
uint32_t ui_theme_color(const struct ui_theme *theme, enum ui_color_role role,
                        const struct ui_album_palette *palette)
{
    (void)palette;
    return theme->color[role];
}
static unsigned ui_theme_duration(unsigned frames)
{
    return global_settings.animations ? frames : 0;
}
static int ui_theme_font_load(const struct ui_theme *theme, enum ui_font_role role, int glyphs)
{
    (void)theme;
    (void)glyphs;
    return font_load(role == UI_FONT_LYRICS_FOCUS ? "26-test" : "18-test");
}
static bool art_present, cached_art;
static int raw_art_reads, art_handle=1;
static fb_data art_pixels[256];
static int album_art_current(void){return art_present?art_handle:-1;}
static void album_palette_refresh(void){}
static unsigned album_palette_generation(void){return 0;}
static bool album_palette_snapshot(struct ui_album_palette *p){*p=(struct ui_album_palette){0};return false;}
static bool album_art_render_background(fb_data *dst,int width,int height,int handle,
                                        bool blur,unsigned foreground)
{
    assert(width==320 && height==240 && blur && foreground==LCD_WHITE);
    if (!art_present || handle!=art_handle)
        return false;
    if (!cached_art)
    {
        raw_art_reads++;
        precise_extra+=5000;
    }
    for (int i=0;i<width*height;i++)
        dst[i]=color_blend(0,art_pixels[0],141);
    return true;
}
static void backlight_set_timeout(int n){(void)n;}static void backlight_set_timeout_plugged(int n){(void)n;}
static void remote_cn(const char *id, unsigned position, unsigned rate)
{
    char record[192];
    int n=snprintf(record,sizeof record,"CN\t9\t%u\t1\t10000\t%u\t%u\t0\t0\t0\t%s\t-\t181818\tRemote\tArtist\tAlbum",
                   remote_clock.seq+1,position,rate,id);
    assert(cider_state_apply(&remote_clock,record,n,current_tick, HZ));
}
static struct wps_state *get_wps_state(void)
{
    if (remote_source && timed_fixture)
    {
        snprintf(playing.path,sizeof playing.path,"cider/%s",remote_clock.id);
        playing.elapsed=cider_playback_position(&remote_clock,current_tick,HZ);
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
static bool cider_available(void){return remote_source;}
static uint32_t cider_wps_clock(long *sample){*sample=remote_clock.position;return remote_clock.seq;}
static const char *cider_remote_name(void){return "Rufin";}
static int audio_status(void){return playing_audio?AUDIO_STATUS_PLAY:0;}
static struct mp3entry *audio_current_track(void){return playing_audio?&playing:NULL;}
static void wps_lyrics_backdrop(void){for(int i=0;i<320*240;i++)screen_pixels[i]=0x1234;}
static void wps_do_playpause(bool b){(void)b;pauses++;wps.paused=!wps.paused;}
static void wps_lyrics_skip(int d){skips++;assert(d==1);char*leaf=strrchr(playing.path,'/');assert(leaf);strcpy(leaf+1,"other.wav");playing.title="Second";playing.artist="Artist2";playing.elapsed=0;}
static void wps_lyrics_seek(int action, void (*draw)(void))
{
    (void)action;
    seeks++;
    wps.ff_rewind_count = 500;
    draw();
    if (plan == 8) {
        playing.elapsed += wps.ff_rewind_count;
        seek_serial++;
    }
    wps.ff_rewind_count = 0;
}
static void record_seek(long target){line_seeks++;seek_target=target;seek_updates=updates;}
static void audio_ff_rewind(long target){assert(!remote_source);record_seek(target);}
static void audio_seek_now(long target){assert(!remote_source);fast_seeks++;record_seek(target);}
static unsigned audio_seek_serial(void){return seek_serial;}
static void cider_control(const char*c,int v){
    assert(remote_source);
    if (!strcmp(c,"seek")) record_seek(v);
    else {assert(v==0);assert(!strcmp(c,"toggle")||!strcmp(c,"next")||!strcmp(c,"previous"));controls++;}
}
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
static int get_action_statuscode(int*b){if(b)*b=select_button?select_button:remote_key?BUTTON_REMOTE:0;return 0;}
static void record_multimedia(int action) { assert(activities==1);multimedia_count++;multimedia_last=action; }
static bool cider_wps_action(int action){
    if (!remote_source) return false;
    record_multimedia(action);return true;
}
static int default_event_handler(int a){
    if (a & BUTTON_MULTIMEDIA) {assert(!remote_source);record_multimedia(a);}
    else {assert(clock_refs==1&&claims==releases);default_events++;}
    return a;
}
static int get_action(int c,int timeout);
'''
retained_header=(r/'apps-ipod/ui/retained.h').read_text()
retained_header=re.sub(r'^#include ".*\n','',retained_header,flags=re.M)
adapter=adapter.replace('RETAINED_TEST_HEADER',retained_header)
# Avoid libc sleep's POSIX signature in this host adapter.
pre=pre.replace('#include <unistd.h>','#include <unistd.h>\n#include <limits.h>\n#define sleep ui_sleep')
post=r'''
static uint32_t rng = 1;
static unsigned pixel_rand(void)
{
    rng = rng * 1664525u + 1013904223u;
    return rng >> 8;
}
static fb_data ref_mix(fb_data a, fb_data b, int p)
{
    unsigned w = (p + 4) >> 3;
    return ((((a >> 11) * (32-w) + (b >> 11) * w) >> 5) << 11)
         | (((((a >> 5 & 63) * (32-w) + (b >> 5 & 63) * w) >> 5)) << 5)
         | (((a & 31) * (32-w) + (b & 31) * w) >> 5);
}
static void test_pixels(void)
{
    /* Wrapped rows share the selected edge and the block's scale origin. */
    assert(lv_text_x(200,200,256,0) == 16 && lv_text_x(100,200,256,0) == 16);
    assert(lv_text_x(200,200,256,2) + 200 == 304);
    assert(lv_text_x(100,200,256,2) + 100 == 304);
    assert(lv_text_x(100,200,256,1) == 110);
    assert(lv_text_x(220,200,282,0) == 6 && lv_text_x(110,200,282,0) == 6);
    assert(lv_text_x(220,200,282,2) + 220 == 314);
    assert(lv_text_x(110,200,282,2) + 110 == 314);
    /* Compare the feather and filtered strips with scalar pixel math. */
    for (int t = 0; t < 2000; t++) {
        unsigned char upper[322] = {0}, lower[322] = {0};
        fb_data pixels[320], ref[320], fg = pixel_rand();
        int width = t % 3 == 0 ? 288 : pixel_rand() % 289;
        int step = t % 2 ? 256 : 232, fy = pixel_rand() % 16;
        int sweep = (int)(pixel_rand() % 322) - 1, alpha = pixel_rand() % 257;
        int dim = alpha * (pixel_rand() % 257) / 256;
        for (int x = 0; x < 320; x++) {
            upper[x + 1] = t % 4 ? pixel_rand() % 65 : 64;
            lower[x + 1] = t % 4 ? pixel_rand() % 65 : 0;
            pixels[x] = ref[x] = pixel_rand();
        }
        for (int x = 0; x < width; x++) {
            int q = (step - 256) / 2 + step * x;
            int ix = (q >> 8) + 1, fx = (q >> 4) & 15;
            unsigned ink = step == 256 ? upper[x + 1] :
                ((upper[ix] * (16-fx) + upper[ix + 1] * fx) * (16-fy)
                + (lower[ix] * (16-fx) + lower[ix + 1] * fx) * fy + 128) / 256;
            int light = sweep < 0 ? alpha : dim + (alpha - dim) * MAX(0, MIN(12, sweep - x)) / 12;
            ref[x] = ref_mix(ref[x], fg, light * ink / 64);
        }
        lv_ink_row(pixels, upper + 1, lower + 1, width, step, fy, alpha, sweep, dim, lv_expand(fg));
        assert(!memcmp(ref, pixels, sizeof ref));
    }
    struct lv_state state={0};
    lv_state=&state;lv.snapshot=app_area;lv.layer=app_area+LV_PIXELS;
    lv.background=lv.layer+LV_PIXELS;lv.art_ready=true;
    for(int i=0;i<3*LV_PIXELS;i++)app_area[i]=pixel_rand();
    for(int alpha=0;alpha<=256;alpha++) {
        int shift=24*(256-alpha)/256;
        lv_present(alpha,shift,lv.layer,lv.snapshot);
        for(int i=0;i<LV_PIXELS;i++) {
            int j=i-shift*LCD_WIDTH;
            fb_data page=j<0?lv.background[i]:lv.layer[j];
            fb_data dark=ref_mix(lv.snapshot[i],LCD_BLACK,alpha*110/256);
            assert(screen_pixels[i]==ref_mix(dark,page,alpha));
        }
        lv_blend_track(alpha);
        for(int i=0;i<LV_PIXELS;i++)
            assert(screen_pixels[i]==ref_mix(lv.snapshot[i],lv.layer[i],alpha));
    }
    lv_state=NULL;
    puts("PASS FU102 pixel reference: 2000 ink scanlines and all 257 transition weights");
}
static const int multimedia_keys[] = {
    BUTTON_MULTIMEDIA_PLAYPAUSE, BUTTON_MULTIMEDIA_NEXT, BUTTON_MULTIMEDIA_PREV,
    BUTTON_MULTIMEDIA_VOLUME_UP, BUTTON_MULTIMEDIA_VOLUME_UP | BUTTON_REPEAT,
    BUTTON_MULTIMEDIA_VOLUME_DOWN, BUTTON_MULTIMEDIA_VOLUME_DOWN | BUTTON_REPEAT,
};
static int pose_y(int index, uint32_t now)
{
    return lv_pose_at(index - lv.pose_first, lv_motion_age(now)).from_y;
}
static int select_scenario(void)
{
    int focus=backward_seek?0:1;
    if (calls==1) {
        if (remote_source) remote_key=false; /* Wheel still scrolls remote lyrics. */
        return (remote_key!=backward_seek) ? ACTION_WPS_VOLDOWN : ACTION_WPS_VOLUP;
    }
    if (calls==2) {
        assert(lv.mode.manual && lv.mode.focus==focus);
        assert(pose_y(focus, lv_clock()) != LV_ANCHOR);
        select_button=(remote_key || remote_source)?BUTTON_RC_SELECT:BUTTON_SELECT;
        return ACTION_UNKNOWN;
    }
    long expected=backward_seek?0:(remote_source ? 2000 : 4000)-global_settings.lyric_lead;
    assert(line_seeks==1 && seek_target==expected);
    assert(!lv.mode.manual && lv.mode.focus==focus);
    assert(lv.pose_focus == focus && lv.motion_frames == 0);
    if (calls==3) {
        assert(updates >= seek_updates + 1 && lv.track_fade);
        assert(lv.elapsed==expected && playing.elapsed!=(unsigned long)expected);
        if (remote_source) remote_cn("one",backward_seek?2800:100,0);
        else playing.elapsed=seek_target; /* An unconfirmed local clock cannot win. */
        select_button=0;
        return ACTION_WPS_BROWSE; /* Release must not exit or send another seek. */
    }
    if (calls==4) {
        assert(lv.elapsed==expected && lv.track_fade);
        long landed=MAX(0,seek_target-100);
        if (remote_source) remote_cn("one",landed,0);
        else {playing.elapsed=landed;seek_serial++;}
        return ACTION_NONE;
    }
    if (calls==5 && backward_seek) {
        remote_cn("one",2800,0); /* Late pre-seek clock after landing. */
        return ACTION_NONE;
    }
    assert(lv_lyric_elapsed()==(backward_seek?200:remote_source?2000:4000));
    assert(pose_y(focus, lv_clock()) == LV_ANCHOR);
    assert(!title_draws&&!manual_draws);
    return ACTION_WPS_BROWSE; /* SELECT exits again after auto-follow. */
}
static int get_action(int c,int timeout){
 static long pause_position;
 assert(shared_scene || !memcmp(panel,screen_pixels,sizeof panel));
 assert(!entry_pinned);
 assert(clock_refs == (lv.boost ? 2 : 1));
 if(timeout>HZ/30)assert(!lv.boost);
 if(plan==15 || plan==16) {
   const unsigned skipped=ANIM_F(15), midway=ANIM_F(5);
   switch(++calls) {
    case 1:
      assert(lv.entry_handle && lv.entry_frame==0 && timeout==1 && updates==1);
      return plan==16 ? SYS_USB_CONNECTED : ACTION_WPS_VOLUP;
    case 2:
      assert(lv.entry_handle && lv.entry_frame==0 && timeout==1 && updates==1);
      assert(lv.mode.manual && lv.mode.focus==1);
      precise_extra+=ui_frame_clock(midway);
      return ACTION_NONE;
    case 3:
      assert(lv.entry_handle && lv.entry_frame==(int)midway && timeout==1 && updates==2);
      assert(lv.motion_frame == midway && pose_y(1, lv_clock()) > LV_ANCHOR);
      precise_extra+=ui_frame_clock(skipped)-ui_frame_clock(midway);
      return ACTION_NONE;
    default:
      assert(calls==4 && !lv.entry_handle && lv.entry_frame==(int)skipped && updates==3);
      return ACTION_WPS_MENU;
   }
 }
 if(plan==14) {
   switch(++calls) {
    case 1: return ACTION_WPS_VOLUP;
    case 2:
      assert(lv.mode.manual && lv.mode.focus==1 && lv.scroll_frames==ANIM_F(20));
      assert(timeout==1 && lv.motion_frame==0 && lv.stagger_frames==ANIM_F(2));
      for (int slot = 0; slot < lv.pose_count; slot++) {
        unsigned delay = slot * ANIM_F(2);
        assert(lv_pose_at(slot, delay).from_y == lv.poses[slot].from_y);
        assert(lv_pose_at(slot, delay + 1).from_y < lv.poses[slot].from_y);
        assert(lv_pose_at(slot, delay + ANIM_F(20)).from_y == lv.poses[slot].to_y);
      }
      precise_extra+=ui_frame_clock(ANIM_F(5));
      retarget_position=pose_y(0, lv_clock());
      assert(retarget_position < LV_ANCHOR);
      return ACTION_WPS_VOLDOWN;
    case 3:
      assert(lv.mode.focus==0 && pose_y(0, lv_clock())==retarget_position);
      assert(lv.motion_frame==0 && timeout==1);
      precise_extra+=ui_frame_clock(ANIM_F(40));
      return ACTION_NONE;
    default:
      assert(calls==4 && pose_y(0, lv_clock())==LV_ANCHOR && !lv.boost);
      return SYS_USB_CONNECTED;
   }
 }
 if(plan==8) {
   calls++;
   if(calls==1){assert(lv.masks && lv.raster);return ACTION_WPS_SKIPNEXT;}
   if(calls==2){assert(lv.track_fade);current_tick+=6;return ACTION_WPS_SEEKFWD;}
   assert(calls==3&&lv.track_fade);
   /* The destination after a mid-fade seek must match a fresh full redraw. */
   lv_draw(true,LV_DRAW_ONLY);
   assert(!memcmp(lv_frame(),lv.layer,LV_FRAME));
   memcpy(lv_frame(),panel,LV_FRAME);
   return ACTION_WPS_MENU;
 }
 if(plan==7||plan==9) {
   assert(calls < 2);
   if(!calls++){assert(!lv.boost); return ACTION_WPS_VOLUP;}
   assert(lv.boost&&lv.mode.manual);
   return plan==7 ? SYS_USB_CONNECTED : nav_action;
 }
 assert(c==CONTEXT_WPS&&timeout>=0&&timeout<=50);current_tick+=MAX(1, timeout);calls++;assert(calls<50);
 if(plan==10) {
  if(calls>1)assert(multimedia_count==calls-1 && multimedia_last==multimedia_keys[calls-2]);
  return (unsigned)calls<=ARRAYLEN(multimedia_keys) ? multimedia_keys[calls-1] : ACTION_WPS_MENU;
 }
 if(plan==11) return select_scenario();
 if(plan==12) {
  if(calls==1) return ACTION_WPS_VOLDOWN; /* Clamp focus to first line. */
  assert(calls==2 && lv.mode.manual && lv.mode.focus==0);
  return ACTION_WPS_BROWSE;
 }
 if(plan==13) {
  assert(perf.first<60000 && perf.entry_prep_us>=1000000);
  return ACTION_WPS_MENU;
 }
 if(plan==0){
  if(calls==1){assert(lv.mode.focus==0);return ACTION_WPS_VOLUP;}
  if(calls==2){assert(lv.mode.manual&&lv.mode.focus==1);retarget_position=pose_y(0, lv_clock());return ACTION_WPS_VOLUP;}
  if(calls==3){assert(pose_y(0, lv_clock())==retarget_position && lv.mode.focus==2);current_tick+=400;playing.elapsed=9000;return ACTION_NONE;}
  if(calls==4){assert(!lv.mode.manual&&lv.mode.focus==2);return ACTION_WPS_PLAY;}
  if(calls==5)return ACTION_WPS_SEEKFWD;
  if(calls==6)return ACTION_WPS_SKIPNEXT;
  if(calls==7){assert(!lyrics_timed()&&lyrics_count()==3&&lv.track_fade);assert(timeout<=1 && "track fade must schedule its next 60 Hz pose");assert(!memcmp(lv_frame(),lv.snapshot,LV_FRAME));current_tick+=20;return ACTION_NONE;}
  assert(!lv.track_fade&&!lv.boost&&lv.mode.focus==0&&!strcmp(lyrics_text(0),"New first line"));return ACTION_WPS_BROWSE;
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
   case 2:assert(!lv.mode.manual&&multimedia_last==ACTION_WPS_VOLDOWN);
     remote_key=false;return ACTION_WPS_VOLUP;
   case 3:assert(lv.mode.manual&&lv.mode.focus==1);return ACTION_WPS_PLAY;
   case 4:return ACTION_WPS_SKIPNEXT;
   case 5:return ACTION_WPS_SKIPPREV;
   default:return ACTION_WPS_LYRICS;
  }
 }
 if(plan==3){
  if(calls==1){assert(lv.pending&&remote_doc.retry&&!lyrics_loaded());wait_row=false;current_tick+=50;return ACTION_NONE;}
  if(lv.pending)return ACTION_NONE;
  assert(lyrics_timed()&&lyrics_loaded()&&lyrics_count()==2&&!strcmp(lyrics_text(0),"星 光"));
  switch(++remote_actions){
   case 1:assert(!lv.paused&&lv.mode.focus==0);paused_width=lv_sweep(0,lv_lyric_elapsed());current_tick+=20;return ACTION_NONE;
   case 2:assert(lv_sweep(0,lv_lyric_elapsed())>=paused_width&&remote_clock.seq==1);
     assert(!lv.art_ready&&!perf.builds);
     art_present=true;art_handle=7;art_pixels[0]=0x07e0;art_generation++;
     pause_position=cider_playback_position(&remote_clock,current_tick,HZ);
     remote_cn("one",pause_position+150,0);return ACTION_NONE;
   case 3:assert(lv.paused&&lv.elapsed==pause_position);
     paused_width=lv_sweep(0,lv_lyric_elapsed());current_tick+=120;return ACTION_NONE;
   case 4:assert(lv.elapsed==pause_position&&lv_sweep(0,lv_lyric_elapsed())==paused_width);
     assert(lv.remote&&lv.art_ready&&perf.builds==1);
     remote_cn("one",1799,0);return ACTION_NONE;
   case 5:assert(lv.mode.focus==0);remote_cn("one",1800,0);return ACTION_NONE;
   case 6:assert(lv.elapsed==1800&&lv.mode.focus==1&&lv.pose_focus==1);return ACTION_WPS_VOLDOWN;
   case 7:assert(lv.mode.manual&&lv.mode.focus==0);current_tick+=400;return ACTION_NONE;
   default:assert(!lv.mode.manual&&lv.mode.focus==1);return ACTION_WPS_LYRICS;
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
   if(calls==2){assert(lv.mode.focus==1&&lv_sweep(1,lv_lyric_elapsed())==0);playing.elapsed+=500;return ACTION_NONE;}
   assert(lv_sweep(1,lv_lyric_elapsed())>0);
   unsigned before=width_reads;
   assert(lv_sweep(1,lv_lyric_elapsed())>0 && width_reads==before);
   return ACTION_WPS_LYRICS;
 }
 if(plan==5){
   if(calls==1){
     assert(lv.art_ready&&perf.builds==1&&raw_art_reads==0);
     assert(lv.background[120*320]==color_blend(0,0xffff,141));
     for(int y=0;y<240;y++)
       assert(lv.background[y*320]==lv.background[120*320] && "cover edges must match its uniform center");
     idle_updates=updates;art_handle=4;art_generation++;
     return ACTION_NONE;
   }
   if(calls==2){
     assert(updates==idle_updates&&perf.builds==1);
     cached_art=false;art_handle=2;art_pixels[0]=0x001f;art_generation++;
     return ACTION_WPS_SKIPNEXT;
   }
   if(calls==3){
     assert(lv.art_ready&&perf.builds==2&&perf.build==5000&&raw_art_reads==1&&lv.track_fade);
     for(int y=0;y<240;y++) assert(lv.background[y*320]==color_blend(0,0x001f,141));
     assert(!memcmp(lv_frame(),lv.snapshot,LV_FRAME));
     current_tick+=25;return ACTION_NONE;
   }
   if(calls==4){
     assert(lv.art_ready&&!lv.track_fade&&perf.builds==2&&raw_art_reads==1);
     art_present=false;strcpy(strrchr(playing.path,'/')+1,"uncovered.wav");return ACTION_NONE;
   }
   assert(!lv.art_ready&&lv.track_fade&&perf.builds==2);
   return ACTION_WPS_MENU;
 }
 assert(!lv.snapshot&&!lv.claimed&&!lyrics_loaded());return SYS_USB_CONNECTED;
}
int main(int argc,char**argv){assert(argc==3);assert(!chdir(argv[1]));
 assert(!atexit(check_trace_closed));
 assert(lv_mix(0,65535,128)==0x7bef);
 assert(lv_mix(0,0xf800,128)==0x7800&&lv_mix(0,0x07e0,128)==0x03e0&&lv_mix(0,0x001f,128)==0x000f);
 art_present=true;for(int i=0;i<256;i++)art_pixels[i]=i<16?0x001f:0xf800;
 art_present=false;
 assert(lv_mix(0x1234,0xabcd,0)==0x1234&&lv_mix(0x1234,0xabcd,256)==0xabcd);
 FILE*f=fopen("song.lrc","w");assert(f);fputs("[00:00.00]One\n[00:04.00]<00:04.00>Two <00:06.00>words\n[00:08.00]Three\n",f);fclose(f);
 f=fopen("other.txt","w");assert(f);fputs("New first line\nNew second line\n中文第三行",f);fclose(f);
 snprintf(playing.path,sizeof playing.path,"%s/song.wav",argv[1]);playing.title="Title";playing.artist="Artist";playing.length=60000;
 global_settings.lyric_lead=200;global_settings.fg_color=65535;global_settings.lyric_highlight=true;global_settings.lyric_backlight=true;global_settings.lyric_prev_opacity=global_settings.lyric_next_opacity=60;
 global_settings.animations=true;
 deny_entry_core=true;
 if (!strcmp(argv[2],"frames")) {
   plan=14;global_settings.animations=deny_entry_core=true;
   assert(lyric_viewer()==GO_TO_ROOT && calls==4);
   plan=7;calls=0;
   assert(lyric_viewer()==GO_TO_ROOT && calls==2);
   assert(clock_refs==1 && boost_gets==boost_puts && releases==2 && default_events==2);
   puts("PASS 20 f staggered line motion, immediate retarget, elapsed-time skip and active USB exit");
   return 0;
 }
 if (!strcmp(argv[2],"entry-frames")) {
   plan=15;deny_entry_core=false;
   assert(lyric_viewer()==GO_TO_PREVIOUS && calls==4 && entry_allocs==1 && entry_frees==1);
   plan=16;calls=updates=0;precise_extra=0;
   assert(lyric_viewer()==GO_TO_ROOT && calls==1 && entry_allocs==2 && entry_frees==2);
   assert(clock_refs==1 && boost_gets==boost_puts && releases==2 && default_events==1);
   puts("PASS opt-in 13 f WPS entry, input and USB on frame zero, late sampling and buffer lifetime");
   return 0;
 }
 if (!strcmp(argv[2],"headset")) {
   plan=10;
   for (int i=0;i<2;i++) {
     remote_source=i;calls=multimedia_count=0;
     if (remote_source) strcpy(playing.path,"cider/song");
     assert(lyric_viewer()==GO_TO_PREVIOUS && multimedia_count==ARRAYLEN(multimedia_keys));
     lyrics_remote_cancel();
   }
   puts("PASS FU70 headset: local and remote dispatch of seven Mikey events without exit");
   return 0;
 }
 if (!strcmp(argv[2],"select")) {
   plan=11;remote_source=timed_fixture=false;
   int lead_settings[]={200,0,-200};
   for (int i=0;i<3;i++) {
     global_settings.lyric_lead=lead_settings[i];
     calls=line_seeks=title_draws=manual_draws=0;playing.elapsed=0;
     snprintf(playing.path,sizeof playing.path,"%s/song.wav",argv[1]);
     assert(lyric_viewer()==GO_TO_PREVIOUS && calls==5 && line_seeks==1);
   }
   assert(fast_seeks==3);
   global_settings.lyric_lead=200;
   plan=12;
   const char *names[]={"song.wav","other.wav"};
   for (int i=0;i<2;i++) {
     calls=line_seeks=0;playing.elapsed=0;
     snprintf(playing.path,sizeof playing.path,"%s/%s",argv[1],names[i]);
     assert(lyric_viewer()==GO_TO_PREVIOUS && calls==2 && line_seeks==0);
   }
   plan=11;remote_source=timed_fixture=remote_key=true;
   const char ready[]="CA\t9\t1\tOK";
   assert(cider_state_apply(&remote_clock,ready,sizeof ready-1,current_tick, HZ));
   remote_cn("one",0,0);
   get_wps_state();
   assert(lyrics_remote_step());
   assert(lyrics_remote_step());
   calls=line_seeks=0;
   assert(lyric_viewer()==GO_TO_PREVIOUS && calls==5 && line_seeks==1);
   backward_seek=true;
   remote_cn("one",2800,0);
   calls=line_seeks=0;
   assert(lyric_viewer()==GO_TO_PREVIOUS && calls==6 && line_seeks==1);
   lyrics_remote_cancel();
   puts("PASS FU71 SELECT: wheel/dock press, cross-fade, local/remote stale clocks, both directions, lead and exit");
   return 0;
 }
 if (!strcmp(argv[2],"entry")) {
   plan=13;remote_source=timed_fixture=remote_key=false;
   reject_loading=verify_early_frame=true;calls=loads=updates=0;
   shared_scene=true;
   deny_entry_core=false;
   snprintf(playing.path,sizeof playing.path,"%s/song.wav",argv[1]);
   assert(lyric_viewer()==GO_TO_PREVIOUS);
   assert(entry_allocs==0);
   puts("PASS early frame before font load and no nested legacy transition");
   return 0;
 }
 if (!strcmp(argv[2],"background")) {
   plan=5;art_present=cached_art=true;art_pixels[0]=0xffff;
   assert(lyric_viewer()==GO_TO_PREVIOUS);
   assert(lyrics_edge_opacity(31,240)<256 && lyrics_edge_opacity(120,240)==256);
   puts("PASS FU71 cached and cold cover edges preserve uniform darkening and text fade");
   return 0;
 }
 if (strcmp(argv[2],"all")) return 1;
 reject_loading=true;
 wps_lyrics_backdrop();assert(lyric_viewer()==GO_TO_PREVIOUS);assert(pauses==1&&seeks==1&&skips==1);
 assert(claims==1&&releases==1&&loads==1&&unloads==1&&!holds&&!activities&&!inhibited&&!lyrics_loaded());
 plan=1;calls=0;remote_source=true;playing.elapsed=0;strcpy(playing.path,"cider/song");
 get_wps_state();current_tick+=3;assert(lyrics_remote_step());assert(lyrics_remote_step());
 assert(lyrics_loaded()&&lyrics_count()==16&&remote_doc.first_ms==30&&!remote_doc.done);
 assert(lyric_viewer()==GO_TO_PREVIOUS&&controls==3&&remote_at==75);
 assert(lyrics_loaded()&&remote_doc.done&&remote_doc.complete_ms>=remote_doc.first_ms);
 lyrics_remote_cancel();
 assert(claims==2&&releases==2&&loads==2&&unloads==2&&!holds&&!activities&&!inhibited);
 plan=2;calls=0;remote_source=false;playing_audio=false;wps.id3=NULL;deny_arena=true;
 assert(lyric_viewer()==GO_TO_ROOT&&default_events==1&&releases==2&&!holds&&!activities&&!inhibited);
 assert(nothing_playing_draws);
 deny_arena=false;playing_audio=true;wps.id3=&playing;
 plan=5;calls=0;art_present=cached_art=true;art_pixels[0]=0xffff;
 snprintf(playing.path,sizeof playing.path,"%s/song.wav",argv[1]);
 assert(lyric_viewer()==GO_TO_PREVIOUS&&releases==3&&!lv_state);
 assert(no_lyrics_draws);
 FILE *metrics=fopen("lyrics-perf.txt","r");assert(metrics);
 char report[1024];struct lyrics_perf_history log=lyrics_perf_scan(fileno(metrics));
 assert(log.count==4&&log.session==4&&log.latest>0);
 assert(!fseek(metrics,log.latest,SEEK_SET));
 size_t len=fread(report,1,sizeof(report)-1,metrics);fclose(metrics);report[len]=0;
 assert(!strncmp(report,"Session 4\nUptime (s): ",22)&&strstr(report,"Duration (ms): "));
 assert(strstr(report,"Source: Local\nTrack: Untimed\n"));
 assert(strstr(report,"art background renders: 2\n")&&strstr(report,"blur max ms: 5\n"));
 assert(strstr(report,"SELECT to first ms: 0\n")&&strstr(report,"animation fps: "));
 assert(strstr(report,"restore: ")&&strstr(report,"text/glyph: ")&&strstr(report,"blend: ")&&strstr(report,"LCD: ")&&strstr(report,"wait: "));
 assert(strstr(report,"animation boosted/frames: ")&&strstr(report,"LCD submits: ")&&strstr(report,"mask rasters: "));
 assert(strstr(report,"entry: ")&&!strstr(report,"exit: ")&&strstr(report,"track change: ")&&strstr(report,"CPU MHz"));
 timed_fixture=true;remote_source=true;remote_key=false;
 const char ca[]="CA\t9\t1\tOK";
 assert(cider_state_apply(&remote_clock,ca,sizeof ca-1,current_tick, HZ));
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
 test_pixels();
 calls=0;plan=7;playing.elapsed=0;
 assert(lyric_viewer()==GO_TO_ROOT && clock_refs==1 && boost_gets==boost_puts);
 plan=8;calls=0;
 arena_bytes = 3 * LV_FRAME + LV_LAYOUTS * sizeof(struct lv_layout)
             + (LV_MASKS + 1) * (sizeof(struct lv_mask) + LV_STRIP_STRIDE * 18)
             + LCD_WIDTH * 18 * sizeof(fb_data);
 assert(lyric_viewer()==GO_TO_PREVIOUS && clock_refs==1 && boost_gets==boost_puts);
 const int nav_actions[] = {ACTION_AGENTS_SWITCH, ACTION_AGENTS_REMOTE_SWITCH,
                            ACTION_AGENTS_COVERS, ACTION_AGENTS_REMOTE_COVERS};
 for (int i=0;i<4;i++) {
   plan=9;calls=0;nav_action=nav_actions[i];
   int previous_claims=claims, previous_releases=releases;
   assert(lyric_viewer()==GO_TO_PREVIOUS && !lv_state);
   assert(nav_requests==i+1 && nav_screen==(i<2 ? AGENTS_NAV_AGENTS : AGENTS_NAV_COVERS));
   assert(nav_remote==(bool)(i%2));
   assert(clock_refs==1 && boost_gets==boost_puts);
   assert(claims==previous_claims+1 && releases==previous_releases+1);
 }
 puts("PASS FU68 four navigation exits during active animation release boost and arena");
 puts("PASS FU65 incremental track layer, single-panel damage and small arena");
 puts("PASS FU65 boost ownership, idle, seek, track change, active USB exit and cue width reuse");
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
p=o/'viewer.c';p.write_text(pre+h+doc+'\n#include "lyrics_motion.h"\n#include "cider_model.h"\nenum {'+','.join(tokens)+'};\n'+button_codes+'\n'+adapter+retained+source+history_fixture+post)
playback=r/'apps-ipod/screens/playback'
subprocess.run(['cc','-m32','-std=gnu11','-Wall','-Wextra','-Werror','-Wno-unused-function',
                '-g','-fsanitize=address,undefined','-fno-pie','-no-pie','-ffunction-sections','-Wl,--gc-sections',
                '-I'+str(r/'apps-ipod'),'-I'+str(r/'apps-ipod/viewers/lyric_viewer'),'-I'+str(playback),
                '-I'+str(next(r.glob('firmware/**/podlink.h')).parent),
                str(p),str(r/'apps-ipod/viewers/lyric_viewer/lyrics_motion.c'),str(r/'apps-ipod/ui/animation.c'),
                str(playback/'cider_model.c'),'-o',str(o/'viewer')],check=True)
for scenario in sys.argv[2:] or ['headset', 'select', 'entry', 'background', 'frames', 'entry-frames', 'all']:
    fixture = o / scenario
    fixture.mkdir()
    subprocess.run([str(o/'viewer'),str(fixture),scenario],check=True)
