/* Production motion/policy tests, independent of Rockbox. GPL v2+. */
#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include "lyrics_motion.h"
int main(void)
{
    assert(lyrics_ease(-1) == 0 && lyrics_ease(0) == 0);
    assert(lyrics_ease(128) == 224 && lyrics_ease(256) == 256);
    int last = -1;
    for (int p = 0; p <= 256; p++) {
        int now = lyrics_ease(p); assert(now >= last && now <= 256); last = now;
    }
    struct lyrics_motion m = {0};
    lyrics_retarget(&m, 256, 100, 26);
    assert(lyrics_motion_at(&m, 100) == 0);
    assert(lyrics_motion_at(&m, 113) == 224);
    int current = lyrics_motion_at(&m, 110);
    lyrics_retarget(&m, -3 * 256, 110, 26);
    assert(lyrics_motion_at(&m, 110) == current); /* retarget never jumps */
    assert(lyrics_moving(&m, 135) && !lyrics_moving(&m, 136));
    assert(lyrics_motion_at(&m, 136) == -768);
    m = (struct lyrics_motion){0};
    lyrics_retarget(&m, 256, UINT32_MAX - 10, 26);
    assert(lyrics_motion_at(&m, 2) == 224); /* monotonic clock wrap */
    assert(lyrics_motion_at(&m, 15) == 256);
    assert(lyrics_focus_weight(3, 3*256) == 256);
    assert(lyrics_focus_weight(3, 3*256+128) == 128);
    assert(lyrics_focus_weight(4, 3*256+128) == 128);
    assert(lyrics_focus_weight(2, 3*256+128) == 0);
    assert(lyrics_opacity(0,141)==256 && lyrics_opacity(256,141)==141);
    assert(lyrics_opacity(-512,141)==90 && lyrics_opacity(2048,141)==51);
    assert(lyrics_edge_opacity(30,240)==0 && lyrics_edge_opacity(42,240)==128);
    assert(lyrics_edge_opacity(100,240)==256 && lyrics_edge_opacity(234,240)==0);
    assert(lyrics_line_height(18,78,0)==30 && lyrics_line_height(18,78,256)==90);
    assert(lyrics_line_height(18,78,128)==60);
    assert(lyrics_line_y(100,0,30,38)==100);
    assert(lyrics_line_y(100,128,30,38)==83);
    assert(lyrics_line_y(100,256,30,38)==66); /* adjacent center is exactly 100 */
    assert(lyrics_word_progress(999,1000,2000,80)==0);
    assert(lyrics_word_progress(1250,1000,2000,80)==20);
    assert(lyrics_word_progress(1500,1000,2000,80)==40);
    assert(lyrics_word_progress(2000,1000,2000,80)==80);
    assert(lyrics_word_progress(1000,1000,1000,80)==80);
    assert(lyrics_progress_ticks(0,10000,320,100)==4);
    assert(lyrics_progress_ticks(10000,10000,320,100)==50);
    assert(lyrics_progress_ticks(0,120000,320,100)==38);
    struct lyrics_mode mode = {0};
    assert(lyrics_mode_follow(&mode,true,3,100,100) && mode.focus==3);
    lyrics_mode_scroll(&mode,1,100,200,100);
    assert(mode.manual && mode.focus==4);
    assert(!lyrics_mode_follow(&mode,true,8,599,100) && mode.focus==4);
    assert(lyrics_mode_follow(&mode,true,8,600,100) && !mode.manual && mode.focus==8);
    lyrics_mode_scroll(&mode,1,100,700,100);
    assert(!lyrics_mode_follow(&mode,false,20,100000,100) && mode.manual && mode.focus==9);
    assert(lyrics_wheel_step(&mode,-1,100001,100)==1); /* reverse resets burst */
    for (int i=0;i<24;i++) assert(lyrics_wheel_step(&mode,-1,100002+i,100)<=4);
    assert(lyrics_wheel_step(&mode,-1,100026,100)==4);
    assert(lyrics_wheel_step(&mode,-1,101000,100)==1); /* remote slow repeat stays one */
    mode = (struct lyrics_mode){0};
    lyrics_mode_scroll(&mode,-1,10,100,100); assert(mode.focus==0);
    for(int i=0;i<100;i++) lyrics_mode_scroll(&mode,1,10,101+i,100);
    assert(mode.focus==9);
    assert(lyrics_idle_ticks(1000,1200,false,false,false,&(struct lyrics_mode){0},true,0,100)==20);
    assert(lyrics_idle_ticks(1000,5000,false,false,false,&(struct lyrics_mode){0},true,0,100)==50);
    assert(lyrics_idle_ticks(1000,1200,true,false,true,&(struct lyrics_mode){0},true,0,100)==50);
    assert(lyrics_idle_ticks(1000,5000,false,true,false,&mode,true,0,100)==3);
    assert(lyrics_idle_ticks(1000,5000,false,false,true,&mode,true,0,100)==3);
    mode = (struct lyrics_mode){.manual=true,.input=100};
    assert(lyrics_idle_ticks(0,10000,false,false,false,&mode,true,495,100)==5);
    assert(lyrics_idle_ticks(0,10000,false,false,false,&mode,false,495,100)==50);
    puts("PASS lyrics easing, retarget, layout, opacity, UTF-8-safe pixel progress policy, modes, acceleration, idle deadlines");
}
