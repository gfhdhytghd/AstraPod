"""Actual unwinder: zero frames, missing callbacks and ARM/Thumb transitions."""
from pathlib import Path
import subprocess
import sys

R = Path(__file__).resolve().parents[2]
O = Path(sys.argv[1]).resolve()
O.mkdir(parents=True, exist_ok=True)
source = r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "unwarm.h"
static unsigned words,halves,reports;
static int scenario;
static UnwindCallbacks cb;
static Boolean report(void*p,Int32 addr){
 (void)p;assert(addr>1);reports++;
 assert(addr==(scenario>=2?0x1101u:0x1100u));
 if(scenario==3)cb.readH=NULL;
 return TRUE;
}
static Boolean readW(Int32 a,Int32*v){
 words++;
 if(a==0x1000||a==0x1100){*v=0xe8bd8000;return TRUE;} /* ldmia sp!, {pc} */
 if(a==0x2000){*v=scenario==0?1:scenario==1?0x1100:0x1101;return TRUE;}
 if(a==0x2004){*v=scenario>=2?1:0;return TRUE;}
 return FALSE;
}
static Boolean readH(Int32 a,Int16*v){
 halves++;assert(a!=0);
 if(a==0x1100){*v=0xbd00;return TRUE;} /* Thumb pop {pc} */
 return FALSE;
}
static Boolean readB(Int32 a,Int8*v){(void)a;(void)v;return FALSE;}
static void init(void){
 cb=(UnwindCallbacks){.report=report,.readW=readW,.readH=readH,.readB=readB};
 words=halves=reports=0;
}
int main(void){
 init();assert(UnwindStart(0,0x2000,&cb,NULL)==UNWIND_RESET);
 assert(UnwindStart(1,0x2000,&cb,NULL)==UNWIND_RESET);
 assert(!words&&!halves&&!reports);
 assert(UnwindStart(0x1000,0x2000,NULL,NULL)==UNWIND_FAILURE);
 for(int field=0;field<4;field++){
  init();switch(field){case 0:cb.report=NULL;break;case 1:cb.readW=NULL;break;
    case 2:cb.readH=NULL;break;case 3:cb.readB=NULL;break;}
  assert(UnwindStart(0x1000,0x2000,&cb,NULL)==UNWIND_FAILURE);
  assert(!words&&!halves&&!reports);
 }
 init();scenario=0;
 assert(UnwindStart(0x1000,0x2000,&cb,NULL)==UNWIND_TRUNCATED);
 assert(words==2&&!halves&&!reports); /* never report or read T:00000000 */
 init();scenario=1;
 assert(UnwindStart(0x1000,0x2000,&cb,NULL)==UNWIND_TRUNCATED);
 assert(reports==1&&!halves);
 init();scenario=2;
 assert(UnwindStart(0x1000,0x2000,&cb,NULL)==UNWIND_TRUNCATED);
 assert(reports==1&&halves==1);
 init();scenario=3;
 assert(UnwindStart(0x1000,0x2000,&cb,NULL)==UNWIND_IREAD_H_FAIL);
 assert(reports==1&&!halves); /* callback disappears between ARM and Thumb */
 static UnwState state;
 init();UnwInitState(&state,&cb,NULL,0x1101,0x2000);cb.readH=NULL;
 assert(UnwStartThumb(&state)==UNWIND_IREAD_H_FAIL&&!halves);
 init();UnwInitState(&state,&cb,NULL,1,0x2000);
 assert(UnwStartThumb(&state)==UNWIND_RESET&&!halves);
 init();UnwInitState(&state,&cb,NULL,0x1000,0x2000);cb.readW=NULL;
 assert(UnwStartArm(&state)==UNWIND_IREAD_W_FAIL&&!words);
 state.cb=NULL;assert(UnwStartThumb(&state)==UNWIND_IREAD_H_FAIL);
 assert(UnwStartArm(&state)==UNWIND_IREAD_W_FAIL);
 assert(!UnwReportRetAddr(&state,0x1100));
 puts("FU46 unwind: zero/reset frames; all missing callbacks; ARM/Thumb chain; readH lost on transition PASS");
}
'''
(O / 'unwind.c').write_text(source)
cmd = ['cc', '-std=gnu11', '-g', '-Wall', '-Wextra',
       '-Wno-sign-compare', '-fsanitize=address,undefined', '-fno-pie', '-no-pie',
       '-I' + str(R / 'lib/unwarminder'), str(O / 'unwind.c')]
cmd += [str(R / 'lib/unwarminder' / n) for n in
        ['unwarminder.c', 'unwarm.c', 'unwarm_arm.c', 'unwarm_thumb.c', 'unwarmmem.c']]
subprocess.run(cmd + ['-o', str(O / 'unwind')], check=True)
subprocess.run([str(O / 'unwind')], check=True, timeout=10)
