/* Host probe for the on-player motion and projection. See stock_replay.py. */
#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../stock_motion.h"

enum { albums = 24, width = 320, height = 240, tex_size = 128 };
static uint8_t art[albums][tex_size][tex_size][3];
static uint8_t pixels[height][width][3];

static double units(int32_t q)
{
    return q / 65536.0;
}

void replay_textures(const uint8_t *rgb);
bool replay_pixels(struct stock_visible *visible, int n, uint8_t *rgb);

static int sample(const struct stock_flow *flow, uint32_t now,
                  struct stock_visible v[STOCK_MAX_VISIBLE])
{
    int n = stock_sample(flow, now, v);
    assert(n >= 0 && n <= STOCK_MAX_VISIBLE);
    for (int i = 0; i < n; ++i) {
        const struct stock_pose p = v[i].pose;
        assert(p.z >= 40 * 65536 && p.z <= 72.5 * 65536);
        assert(abs(p.x) < 100 * 65536 && v[i].album >= 0 && v[i].album < albums);
        struct stock_edge edge[2];
        stock_project(p, edge);
        for (int j = 0; j < 2; j++) {
            assert(edge[j].depth > 0 && edge[j].bottom > edge[j].top);
            assert(isfinite(units(edge[j].x)) && fabs(units(edge[j].x)) < 1024);
        }
    }
    return n;
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    FILE *textures = fopen(argv[1], "rb");
    assert(textures && fread(art, 1, sizeof(art), textures) == sizeof(art));
    fclose(textures);
    replay_textures(&art[0][0][0][0]);
    struct stock_flow full, half;
    stock_init(&full, albums, 0, 0, false);
    stock_init(&half, albums, 0, 0, false);
    char op;
    unsigned now;
    int arg;
    while (scanf(" %c %u %d", &op, &now, &arg) == 3) {
        struct stock_visible v[STOCK_MAX_VISIBLE], other[STOCK_MAX_VISIBLE];
        if (op == 's') {
            stock_scroll(&full, arg, now);
            stock_scroll(&half, arg, now);
        } else if (op == 'h' || op == 'b') {
            int n = sample(&full, now, v);
            if (op == 'b') {
                assert(sample(&half, now, other) == n);
                for (int i = 0; i < n; i++) {
                    assert(v[i].album == other[i].album);
                    assert(memcmp(&v[i].pose, &other[i].pose, sizeof(v[i].pose)) == 0);
                    struct stock_edge a[2], b[2];
                    stock_project(v[i].pose, a);
                    stock_project(other[i].pose, b);
                    assert(memcmp(a, b, sizeof(a)) == 0);
                }
            }
        } else {
            assert(op == 'f' || op == 'r');
            int n = sample(&full, now, v);
            for (int i = 0; i < n; i++) {
                struct stock_edge edge[2];
                stock_project(v[i].pose, edge);
                printf("%u %d %d %d", now, full.selected, v[i].album, v[i].pose.x);
                for (int j = 0; j < 2; j++)
                    printf(" %d %d %d", edge[j].x, edge[j].top, edge[j].bottom);
                putchar('\n');
            }
            assert(replay_pixels(v, n, &pixels[0][0][0]));
            if (op == 'r') {
                char name[64];
                snprintf(name, sizeof(name), "render-%u.ppm", now);
                FILE *out = fopen(name, "wb");
                assert(out);
                fprintf(out, "P6\n%d %d\n255\n", width, height);
                assert(fwrite(pixels, 1, sizeof(pixels), out) == sizeof(pixels));
                fclose(out);
            }
        }
    }
    return 0;
}
