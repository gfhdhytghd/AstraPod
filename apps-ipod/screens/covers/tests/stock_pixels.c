/* Host simulator build. Supply the carousel's cache with fixture textures,
 * then paint the same projected columns and RGB565 samples as the player. */
#include "../carousel.c"

enum { FIXTURE_ALBUMS = 24, TEX_SIZE = 128 };
static struct fixture_album {
    int offset;
    struct dim dim;
    pix_t texels[TEX_SIZE * TEX_SIZE];
} fixture[FIXTURE_ALBUMS];

void replay_textures(const uint8_t *rgb)
{
#if CONFIG_BUFLIB_BACKEND == BUFLIB_BACKEND_MEMPOOL
    static union buflib_data handles[FIXTURE_ALBUMS];
    buf_ctx.handle_table = handles + FIXTURE_ALBUMS;
#else
    static struct buflib_malloc_handle handles[FIXTURE_ALBUMS];
    buf_ctx.allocs = handles;
#endif
    number_of_slides = FIXTURE_ALBUMS;
    for (int a = 0; a < FIXTURE_ALBUMS; a++) {
        struct fixture_album *f = &fixture[a];
        f->offset = offsetof(struct fixture_album, dim);
        f->dim.width = f->dim.height = TEX_SIZE;
        for (int y = 0; y < TEX_SIZE; y++)
            for (int x = 0; x < TEX_SIZE; x++, rgb += 3)
                f->texels[x * TEX_SIZE + y] = LCD_RGBPACK(rgb[0], rgb[1], rgb[2]);
        pf_sldcache.cache[a].index = a;
        pf_sldcache.cache[a].hid = a + 1;
#if CONFIG_BUFLIB_BACKEND == BUFLIB_BACKEND_MEMPOOL
        buf_ctx.handle_table[-1 - a].alloc = (char *)f;
#else
        handles[a].user = f;
#endif
    }
}

bool replay_pixels(struct stock_visible *visible, int n, uint8_t *rgb)
{
    static pix_t frame[LCD_HEIGHT][LCD_WIDTH];
    memset(frame, 255, sizeof(frame));
    for (int x = 0; x < LCD_WIDTH; x++) {
        flow_columns[x].height = 0;
        flow_top[x] = LCD_HEIGHT;
        flow_end[x] = 0;
    }
    for (int i = 0; i < n; i++) {
        struct flow_card card = { .id = visible[i].album, .linear = visible[i].linear };
        stock_project(visible[i].pose, card.edge);
        flow_surfaces[i].cached_slot = card.id;
        flow_cover_columns(&card, i, 0);
    }
    for (int x = 0; x < LCD_WIDTH; x++)
        flow_draw_column(x, 0, LCD_HEIGHT, &frame[0][x], LCD_WIDTH, NULL);
    /* Compare the clipped spans with drawing every complete face in order. */
    static pix_t whole[LCD_HEIGHT][LCD_WIDTH];
    memset(whole, 255, sizeof(whole));
    for (int i = 0; i < n; i++) {
        const struct flow_face *f = &flow_faces[i];
        for (int x = f->first; x < f->end; x++) {
            int t = f->t0 + (x - f->first) * f->dt;
            int h = f->start_height + ((int64_t)(f->end_height - f->start_height) * t >> 30);
            int y = f->top + ((int64_t)f->top_delta * t >> 30);
            struct flow_column col = { y, h, t, f };
            flow_draw_span(&col, 0, LCD_HEIGHT, &whole[0][x], LCD_WIDTH, NULL);
        }
    }
    for (int y = 0; y < LCD_HEIGHT; y++)
        for (int x = 0; x < LCD_WIDTH; x++) {
            pix_t c = frame[y][x];
            *rgb++ = RGB_UNPACK_RED(c);
            *rgb++ = RGB_UNPACK_GREEN(c);
            *rgb++ = RGB_UNPACK_BLUE(c);
        }
    return memcmp(frame, whole, sizeof(frame)) == 0;
}

#ifdef REPLAY_STOCK_FLIP
static pix_t fixture_panel[256][256];

void replay_flip_texture(const uint8_t *rgb)
{
    for (int y = 0; y < 256; y++)
        for (int x = 0; x < 256; x++, rgb += 3)
            fixture_panel[x][y] = LCD_RGBPACK(rgb[0], rgb[1], rgb[2]);
}

void replay_flip_face(struct stock_pose p, uint8_t *rgb)
{
    static pix_t frame[LCD_HEIGHT][LCD_WIDTH];
    for (int y = 0; y < LCD_HEIGHT; y++)
        for (int x = 0; x < LCD_WIDTH; x++) {
            const uint8_t *at = rgb + (y * LCD_WIDTH + x) * 3;
            frame[y][x] = LCD_RGBPACK(at[0], at[1], at[2]);
        }
    bool back = stock_back_face(p);
    stock_draw_quad(&frame[0][0], p, back ? &fixture_panel[0][0] : fixture[0].texels,
                    back ? 256 : TEX_SIZE, back ? 256 : TEX_SIZE, back ? 256 : TEX_SIZE);
    for (int y = 0; y < LCD_HEIGHT; y++)
        for (int x = 0; x < LCD_WIDTH; x++) {
            pix_t c = frame[y][x];
            *rgb++ = RGB_UNPACK_RED(c);
            *rgb++ = RGB_UNPACK_GREEN(c);
            *rgb++ = RGB_UNPACK_BLUE(c);
        }
}
#endif
