/* Host replay of the five stock button transitions, sampled at capture times. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../stock_motion.h"

static uint8_t art[24][128][128][3], panel_rgb[256][256][3], pixels[240][320][3];
void replay_textures(const uint8_t *rgb);
bool replay_pixels(struct stock_visible *visible, int n, uint8_t *rgb);
void replay_flip_texture(const uint8_t *rgb);
void replay_flip_face(struct stock_pose p, uint8_t *rgb);

static void read_exact(const char *path, void *data, size_t size)
{
    FILE *f = fopen(path, "rb");
    assert(f && fread(data, 1, size, f) == size);
    fclose(f);
}

int main(int argc, char **argv)
{
    assert(argc == 3);
    struct stock_pose edge_on = stock_slot_base(0);
    edge_on.yaw = 90 * STOCK_ONE;
    assert(!stock_back_face(edge_on));
    edge_on.yaw = -90 * STOCK_ONE;
    assert(!stock_back_face(edge_on));
    read_exact(argv[1], art, sizeof(art));
    read_exact(argv[2], panel_rgb, sizeof(panel_rgb));
    replay_textures(&art[0][0][0][0]);
    replay_flip_texture(&panel_rgb[0][0][0]);
    const enum stock_transition transitions[] = { STOCK_TO_BACK, STOCK_TO_FRONT,
        STOCK_PANEL_TO_PLAYING, STOCK_COVER_TO_PLAYING, STOCK_FROM_PLAYING };
    unsigned elapsed, save;
    char op;
    while (scanf(" %c %u %u", &op, &elapsed, &save) == 3) {
        assert(op >= 'A' && op <= 'E');
        struct stock_flow row;
        struct stock_flip flip;
        struct stock_visible sides[STOCK_MAX_VISIBLE];
        if (op == 'E')
            stock_enter(&row, 24, 0, 0, true);
        else
            stock_init(&row, 24, 0, 0, false);
        stock_flip_start(&flip, transitions[op - 'A'], stock_slot_base(0), 0);
        if (op == 'C' || op == 'D')
            stock_leave_to_playing(&row, 0);
        int count = stock_sample(&row, elapsed, sides);
        struct stock_pose p = stock_flip_pose(&flip, elapsed);
        int n = 0;
        for (int i = 0; i < count; ++i) {
            if (sides[i].album)
                sides[n++] = sides[i];
            else if (op == 'E')
                assert(memcmp(&sides[i].pose, &p, sizeof(p)) == 0);
        }
        assert(replay_pixels(sides, n, &pixels[0][0][0]));
        replay_flip_face(p, &pixels[0][0][0]);
        struct stock_edge edges[2];
        stock_project(p, edges);
        printf("%c %u %d %d %d", op, elapsed, n, stock_back_face(p), stock_flip_done(&flip, elapsed));
        for (int j = 0; j < 2; j++) {
            assert(edges[j].depth > 0 && edges[j].top < edges[j].bottom);
            printf(" %d %d %d", edges[j].x, edges[j].top, edges[j].bottom);
        }
        putchar('\n');
        if (save) {
            char name[64];
            snprintf(name, sizeof(name), "%c-%u.ppm", op, elapsed);
            FILE *f = fopen(name, "wb");
            assert(f);
            fprintf(f, "P6\n320 240\n255\n");
            assert(fwrite(pixels, 1, sizeof(pixels), f) == sizeof(pixels));
            fclose(f);
        }
    }
    return 0;
}
