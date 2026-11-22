/* Foreground image and font adapters. GNU GPL version 2 or later. */
#include <limits.h>
#include <stdint.h>
#include "audio.h"
#include "draw/bmp.h"
#include "font.h"
#include "metadata/art_cache.h"
#include "metadata.h"
#include "plugins/rock_compat.h"
#include "plugins/rock_files.h"
#include "plugins/rock_game_io.h"
#include "string-extra.h"

/* Bound the core decoder's signed size calculations and short row fields. */
#define BMP_DIMENSION_LIMIT 4096
#define BMP_HEADER_SIZE 54

static uint32_t bmp_le32(const unsigned char *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 |
           (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static bool bmp_dimensions(int fd, int *width, int *height)
{
    unsigned char header[BMP_HEADER_SIZE];
    if (rock_read(fd, header, sizeof(header)) != sizeof(header) ||
        header[0] != 'B' || header[1] != 'M' ||
        header[26] != 1 || header[27] || header[29])
        return false;

    uint32_t offset = bmp_le32(header + 10);
    uint32_t dib_size = bmp_le32(header + 14);
    uint32_t compression = bmp_le32(header + 30);
    uint32_t depth = header[28];
    int32_t w = (int32_t)bmp_le32(header + 18);
    int32_t h = (int32_t)bmp_le32(header + 22);
    uint32_t colors = bmp_le32(header + 46);
    if (w <= 0 || w > BMP_DIMENSION_LIMIT ||
        h == 0 || h < -BMP_DIMENSION_LIMIT || h > BMP_DIMENSION_LIMIT ||
        dib_size < 40 || dib_size > offset || offset - dib_size < 14)
        return false;
    if (depth != 1 && depth != 4 && depth != 8 && depth != 16 &&
        depth != 24 && depth != 32)
        return false;

    if (compression == 0 && depth <= 8)
    {
        unsigned int palette_size = 1u << depth;
        if ((colors && colors != palette_size) ||
            offset - dib_size - 14 < 4 * palette_size)
            return false;
    }
    else if (compression != 0 &&
             (compression != 3 || (depth != 16 && depth != 32) ||
              (dib_size != 40 && dib_size < 52) ||
              offset < (dib_size >= 56 ? 70u : 66u)))
        return false;

    if (h < 0)
        h = -h;
    uint32_t row = ((w * depth + 31) / 32) * 4;
    off_t length = rock_ffilesize(fd);
    if (length < 0 || (uint64_t)offset + row * h > (uint64_t)length)
        return false;
    *width = w;
    *height = h;
    return true;
}

int rock_read_bmp_fd(int fd, struct bitmap *bm, int maxsize, int format,
                     const struct custom_format *cformat)
{
    int width, height;
    if (rock_usb_taken() || !bm || maxsize < 0 ||
        rock_lseek(fd, 0, SEEK_CUR) != 0 ||
        !bmp_dimensions(fd, &width, &height) ||
        rock_lseek(fd, 0, SEEK_SET) != 0)
        return -1;

    return read_bmp_fd(fd, bm, maxsize, format, cformat);
}

int rock_read_plugin_bmp(const char *path, struct bitmap *bm, size_t size,
                         bool resize)
{
    if (!path || !bm || !bm->data || size > INT_MAX)
        return -1;

    int fd = rock_open(path, O_RDONLY);
    if (fd < 0)
        return -1;

    int width, height;
    int result = -1;
    if (bmp_dimensions(fd, &width, &height) &&
        bm->width > 0 && bm->width <= LCD_WIDTH &&
        bm->height > 0 && bm->height <= LCD_HEIGHT &&
        (resize || (width <= bm->width && height <= bm->height)) &&
        rock_lseek(fd, 0, SEEK_SET) == 0)
    {
        int format = FORMAT_NATIVE;
        if (resize)
            format |= FORMAT_RESIZE | FORMAT_DITHER;
        result = read_bmp_fd(fd, bm, size, format, NULL);
    }
    rock_close(fd);
    return result;
}

int rock_load_plugin_art(struct bitmap *bm, size_t size)
{
    if (rock_usb_taken() || !bm || !bm->data || size > INT_MAX ||
        bm->width <= 0 || bm->width > LCD_WIDTH ||
        bm->height <= 0 || bm->height > LCD_HEIGHT)
        return -1;

    struct mp3entry *track = audio_current_track();
    if (!track || !track->path[0])
        return -1;

    char path[MAX_PATH];
    if (strlcpy(path, track->path, sizeof(path)) >= sizeof(path))
        return -1;
    unsigned int key = art_cache_track_key(path);
    bool fallback;
    if (!art_cache_lookup_hash(key, art_cache_size_index("wps"),
                               path, sizeof(path), &fallback) || fallback)
        return -1;
    int fd = rock_open(path, O_RDONLY);
    if (fd < 0)
        return -1;
    int result = art_cache_load_aat(fd, bm, size, NULL);
    rock_close(fd);
    return result;
}

/* Keep one font reference owned by the foreground module. */
static int plugin_font = -1;

int rock_load_plugin_font(const char *path)
{
    if (rock_usb_taken() || !path || plugin_font >= 0)
        return -1;
    plugin_font = font_load(path);
    if (plugin_font >= 0)
        font_lock(plugin_font, true);
    return plugin_font;
}

void rock_unload_plugin_font(int id)
{
    if (id >= 0 && id == plugin_font)
        rock_release_plugin_font();
}

void rock_release_plugin_font(void)
{
    if (plugin_font < 0)
        return;
    font_lock(plugin_font, false);
    font_unload(plugin_font);
    plugin_font = -1;
}
