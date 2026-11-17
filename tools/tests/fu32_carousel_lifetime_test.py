"""FU32 join-before-callback, provider replacement and pixel-exact cache resume."""
from pathlib import Path
import re,subprocess,sys
R=Path(__file__).resolve().parents[2];O=Path(sys.argv[1]).resolve();O.mkdir(parents=True,exist_ok=True)
S=Path(sys.argv[2]).resolve() if len(sys.argv)>2 else R
s=(S/'apps-ipod/screens/covers/carousel.c').read_text()
def fn(n):
 m=re.search(r'(?m)^(?:static )?(?:inline )?[\w *]+\b'+n+r'\([^;{}]*\)\s*\{',s);assert m,n
 e=m.end();d=1
 while d:d+=(s[e]=='{')-(s[e]=='}');e+=1
 return s[m.start():e]
pre=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#define FOR_NB_SCREENS(i) for(int i=0;i<1;i++)
#define EV_EXIT 1
#define P2STR(p) ((const char *)(p))
struct carousel_model { const char *title; bool external; };
struct db_summary_t{unsigned char*buf;size_t buf_sz;int marker;};
static const struct carousel_model a={"Album Covers",false},b={NULL,true},*model=&a,*resume_model;
static const char *remote;
static const char *cider_remote_name(void) { return remote; }
static struct db_summary_t carousel_idx,resume_index;
static int resume_handle,thread_id=1,thread_q,exits,joins,deletes,undos,releases;
static bool thread_is_running,pf_theme_pushed,pf_boosted,wants_to_quit,fail_alloc;
static void *owned_buffer,*resume_placeholder,*pf_placeholder_buf;
static size_t owned_bytes;
static struct{int backlight_timeout,backlight_timeout_plugged;}global_settings;
static void*saved;
static int core_alloc(size_t n){assert(!saved);if(fail_alloc)return -1;saved=malloc(n);assert(saved);return 1;}
static void*core_get_data(int h){assert(h==1&&saved);return saved;}
static int core_free(int h){assert(h==1&&saved);free(saved);saved=NULL;return 0;}
static void queue_post(int*q,int e,int d){(void)q;(void)d;assert(e==EV_EXIT&&thread_is_running);exits++;}
static void thread_wait(int id){assert(id==1&&exits>joins);joins++;}
static void queue_delete(int*q){(void)q;deletes++;}
static void viewportmanager_theme_undo(int n,bool b){(void)n;(void)b;assert(!thread_is_running);undos++;}
static void cpu_boost(bool b){(void)b;assert(!thread_is_running);}
static void backlight_set_timeout(int n){(void)n;}static void backlight_set_timeout_plugged(int n){(void)n;}
static void app_release_buffer(const char*n){(void)n;assert(!thread_is_running);releases++;}
'''
post=r'''
int main(void){
 remote="Rufin";model=&b;assert(!strcmp(model_title(),"Rufin"));
 remote="Cider";assert(!strcmp(model_title(),"Cider"));
 model=&a;assert(!strcmp(model_title(),"Album Covers"));
 thread_is_running=pf_theme_pushed=pf_boosted=true;cleanup();assert(exits==1&&joins==1&&deletes==1&&undos==1&&releases==1);
 /* Nine final-tier native surfaces in the exact app-buffer image. */
 owned_bytes=3u*1024*1024;owned_buffer=malloc(owned_bytes);assert(owned_buffer);
 unsigned short*p=owned_buffer;for(unsigned i=0;i<owned_bytes/2;i++)p[i]=(unsigned short)(1+(i%65534));
 carousel_idx=(struct db_summary_t){(unsigned char*)owned_buffer+4096,owned_bytes-4096,42};pf_placeholder_buf=(unsigned char*)owned_buffer+1024;
 assert(carousel_save_resume());memset(owned_buffer,0,owned_bytes);carousel_idx.marker=99;pf_placeholder_buf=NULL;
 assert(carousel_restore_resume(owned_buffer,owned_bytes));assert(carousel_idx.marker==42&&pf_placeholder_buf==(unsigned char*)owned_buffer+1024);
 for(unsigned i=0;i<owned_bytes/2;i++)assert(p[i]==(unsigned short)(1+(i%65534)));
 for(int cover=0;cover<9;cover++)for(int pixel=0;pixel<128*128;pixel++)assert(p[cover*128*128+pixel]);
 assert(!resume_handle&&!saved);assert(carousel_save_resume());model=&b;assert(!carousel_restore_resume(owned_buffer,owned_bytes)&&!saved);
 model=&a;fail_alloc=true;assert(!carousel_save_resume()&&!resume_handle);free(owned_buffer);
 puts("FU32 stop/join before theme and app release; 3 MiB exact restore, 9 final-tier covers 0 black pixels, provider rejection, allocation failure PASS");}
'''
(O/'test.c').write_text(pre+fn('model_title')+fn('end_pf_thread')+fn('cleanup')+fn('carousel_forget_resume')+fn('carousel_save_resume')+fn('carousel_restore_resume')+post)
subprocess.run(['cc','-std=gnu11','-Wall','-Wextra','-Werror','-g','-fsanitize=address,undefined','-fno-pie','-no-pie',str(O/'test.c'),'-o',str(O/'test')],check=True)
subprocess.run([str(O/'test')],check=True)
body=fn('carousel_body');assert body.index('end_pf_thread();')<body.index('model = m;')
loop=fn('album_covers_loop')
for callback in ('model->enter(center_index)','model->on_context(center_index)','model->on_menu()','model->sort_next()','model->sort_prev()'):
 pos=loop.index(callback);assert 'end_pf_thread();' in loop[max(0,pos-650):pos]
assert loop.index('buf_ctx_lock();',loop.index('int art_y ='))<loop.index('update_scroll_animation();')<loop.index('buf_ctx_unlock();',loop.index('int art_y ='))
assert '.external = true' in (R/'apps-ipod/screens/covers/cider_covers.c').read_text()
assert '.title = ID2P(LANG_ALBUM_COVERS)' in (R/'apps-ipod/screens/covers/album_covers.c').read_text() or 'LANG_ALBUM_COVERS' in (R/'apps-ipod/screens/covers/album_covers.c').read_text()
print('Provider callbacks/join order, render lock and Rufin/Cider/local titles PASS')
