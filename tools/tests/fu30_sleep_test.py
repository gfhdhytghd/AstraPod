"""Production native timer expiry with an external playback source callback."""
from pathlib import Path
import re,subprocess,sys
r=Path(__file__).resolve().parents[2];o=Path(sys.argv[1]).resolve();o.mkdir(parents=True,exist_ok=True)
def fn(text,name):
 m=re.search(r'(?m)^(?:static )?[\w *]+\b'+name+r'\([^;{}]*\)\s*\{',text);assert m,name
 e=m.end();n=1
 while n:n+=(text[e]=='{')-(text[e]=='}');e+=1
 return text[m.start():e]
power=(r/'firmware/powermgmt.c').read_text();cider=(r/'apps-ipod/screens/playback/cider.c').read_text()
pre=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#define TIME_AFTER(a,b) ((a)>(b))
#define DEBUGF(...)
#define CONFIG_CHARGING 0
static long current_tick=100,sleeptimer_endtick=10;
static bool active,sleep_pause_due;
static int local_pauses,backlights,poweroffs,cancels;
static bool(*sleep_timer_callback)(void);
static bool usb_inserted(void){return false;}
static void audio_pause(void){local_pauses++;}
static void backlight_off(void){backlights++;}
static void sys_poweroff(void){poweroffs++;}
static void set_sleep_timer(int n){assert(!n);cancels++;}
'''
post=r'''
int main(void){set_sleep_timer_callback(cider_sleep_pause);active=true;handle_sleep_timer();assert(sleep_pause_due&&backlights==1&&cancels==1&&!local_pauses&&!poweroffs);
active=false;sleep_pause_due=false;handle_sleep_timer();assert(!sleep_pause_due&&poweroffs==1&&!local_pauses);puts("Native Sleep Timer expiry requests idempotent Cider pause and keeps local expiry policy PASS");}
'''
(o/'test.c').write_text(pre+fn(power,'set_sleep_timer_callback')+fn(cider,'cider_sleep_pause')+fn(power,'handle_sleep_timer')+post)
subprocess.run(['cc','-std=gnu11','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-fno-pie','-no-pie',str(o/'test.c'),'-o',str(o/'test')],check=True);subprocess.run([str(o/'test')],check=True)
