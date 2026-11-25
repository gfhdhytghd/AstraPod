"""Exercise the firmware's pixel loops and time them on the host, over the
refreshes pfgeom records from the 3D view's motion."""
from pathlib import Path
import subprocess
import sys

root = Path(__file__).resolve().parents[2]
out = Path(sys.argv[1]).resolve()
out.mkdir(parents=True, exist_ok=True)
source = (Path(sys.argv[2]) if len(sys.argv) > 2 else
          root / 'apps-ipod/screens/covers/carousel.c').read_text()
geometry = (root / 'tools/pfgeom/pfgeom.c').read_text()

def between(s, start, end):
    return s[s.index(start):s.index(end, s.index(start))]

aa = 'static void build_side_level' in source
album_flip = 'static int pf_neighbor_light' in source
program = '''
#include <assert.h>
#include <stdbool.h>
#include <time.h>
''' + geometry[:geometry.index('static struct dim slide_dim;')] + '''
typedef uint16_t pix_t;
struct art { struct dim size; pix_t data[128*128*2]; };
static struct art image, side_art[2*MAX_SLIDES_COUNT];
static struct dim *surface(struct slide_data *slide) {
    if (slide == &center_slide) return &image.size;
    for (int i=0; i<MAX_SLIDES_COUNT; i++) {
        if (slide == &left_slides[i]) return &side_art[i].size;
        if (slide == &right_slides[i]) return &side_art[MAX_SLIDES_COUNT+i].size;
    }
    abort();
}
static pix_t buffer[320*240];
static unsigned pf_bg_rb, pf_bg_g;
static int pf_neighbor_light __attribute__((unused)) = 256;
#define BUFFER_WIDTH 320
#define NO_INLINE __attribute__((noinline))
#define fabs fabsr
''' + between(source, '#define FLIP_DIST ', '#define DISPLAY_LEFT_R') + between(
    source, '/* n / FLIP_DIST', '/* num / den for') + between(
    geometry, '#define PF_SLOT_X', 'static int slide_x_range') + (
    between(source, 'static inline unsigned int fade_red_blue', 'static pix_t pf_color')
    if 'static inline unsigned int fade_red_blue' in source else '') + between(
    source, 'struct pf_fade\n', '/**\n * Render a single slide') + between(
    source, 'static int slide_voff', '/* The rows a slide') + between(
    source, 'static bool slide_x_range', '/* Slides are drawn in tiles') + between(
    source, '#define PF_TILE_W', 'void set_current_slide') + between(
    source, 'static void cull_side', '/* The rows the covers will disturb') + between(
    geometry, 'struct pose\n', '/* Rows the frame')
if aa:
    program += between(source, 'static void build_side_level', 'static int read_pfraw')
program += '''
int main(void) {
    image.size = (struct dim){128,128};
    pf_half_height=90; pf_lower_half=90;
    recalc_offsets();
    center_index = target = 500;
    coverflow_idle();
    for (int x=0;x<128;x++)
        for (int y=0;y<128;y++)
            image.data[x*128+y]=(x&1)?0xffff:0;
'''
if aa:
    program += '''
    build_side_level(&image.size);
    for (int i=0;i<64*128;i++) assert(image.data[128*128+i]==0x7bef);
    for (int phase=1; phase<8; phase++) {
        pix_t dest[3]={0,0,0};
        struct pf_col col={.px=dest};
        struct pf_extent span=project_extent(1024+phase*128,1024,0xffffffffu/1024);
        cover_half(&col, span, 3, 1, 0xffff, NULL);
        assert(col.n==1 && dest[0]==0 && dest[2]==0);
        int r=dest[1]>>11, g=(dest[1]>>5)&63;
        assert(abs(r-31*phase/8)<=1 && abs(g-63*phase/8)<=1);
    }
    side_art[MAX_SLIDES_COUNT]=image;
    render_slide(&right_slides[0],256);
    int grey=0;
    for (int x=0;x<320;x++) if (buffer[90*320+x]==0x7bef) grey++;
    assert(grey>25);
    memset(image.data,0xff,sizeof(image.data));
    side_art[MAX_SLIDES_COUNT]=image;
    memset(buffer,0,sizeof(buffer));
    render_slide(&right_slides[0],256);
    int partial=0;
    for (int x=0;x<320;x++) {
        int y=0;
        while(y<90 && !buffer[y*320+x]) y++;
        if(y==90 || buffer[y*320+x]==0xffff) continue;
        assert(buffer[(y+1)*320+x]==0xffff);
        assert(buffer[(179-y)*320+x]==buffer[y*320+x]);
        partial++;
    }
    assert(partial>25);
'''
program += '''
    for (int j=0;j<128*128*2;j++) image.data[j]=(pix_t)(37*j);
    for (int i=0;i<2*MAX_SLIDES_COUNT;i++) {
        side_art[i].size=image.size;
        for(int j=0;j<128*128*2;j++) side_art[i].data[j]=(pix_t)(37*j+i);
    }
    volatile unsigned checksum=0;
    record_scenarios();
    for(int pass=0;pass<7;pass++) {
        clock_t start=clock();
        for(int q=0;q<n_poses;q++) {
            pose(&poses[q]);
            memset(buffer,0,sizeof buffer);
            coverflow_render();
            checksum+=buffer[90*320+102];
        }
        printf("%.3f ",1e6*(clock()-start)/CLOCKS_PER_SEC/n_poses);
    }
    printf("us/frame checksum=%u\\n",checksum);
}
'''
if album_flip:
    program = program.replace('coverflow_render();', 'coverflow_render(true);')
cfile = out / 'coverflow_pixels.c'
cfile.write_text(program)
binary = out / 'coverflow_pixels'
subprocess.run(['cc', '-O2', '-std=gnu11', '-fwrapv', str(cfile), '-o',
                str(binary), '-lm'], check=True)
subprocess.run([str(binary)], check=True)
