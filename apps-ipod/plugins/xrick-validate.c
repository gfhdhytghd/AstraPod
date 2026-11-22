/* XRick stock-version resource relationships. GNU GPL version 2 or later. */
#include "xrick-compat.h"
#include "xrick/draw.h"
#include "xrick/data/sprites.h"
#include "xrick/ents.h"
#include "xrick/maps.h"
#include "xrick/screens.h"
#include "xrick/data/tiles.h"

bool xrick_resource_path(const char *name, size_t length)
{
    /* Graphics for the other stock display format have empty entries. */
    if (!length)
        return true;
    if (length > 96)
        return false;

    bool first = true;
    for (size_t i = 0; i < length; i++)
    {
        unsigned char c = name[i];
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '_' || c == '-')
            first = false;
        else if (!first && c == '/')
            first = true;
        else if (first || c != '.')
            return false;
    }
    return !first;
}

static bool tile_text(const U8 *text, unsigned cols, unsigned rows)
{
    unsigned width = 0;
    for (; *text != 0xfe; text++)
    {
        if (*text == 0xff)
        {
            if (--rows == 0)
                return false;
            width = 0;
        }
        else if (++width > cols)
            return false;
    }
    return true;
}

static bool intro_valid(size_t index)
{
    const U8 *title = screen_imaptext[index];
    size_t width = 0;
    while (*title < 0xfe && ++width <= 35)
        title++;
    if (*title != 0xff || !tile_text(title + 1, 35, 12))
        return false;

    size_t step = screen_imapsofs[index];
    if (step >= screen_nbr_imapstesps)
        return false;

    for (step++; step < screen_nbr_imapstesps; step++)
    {
        const screen_imapsteps_t *entry = &screen_imapsteps[step];
        if (!entry->count)
            return true;
        size_t sprite = entry->base;
        if (sprite >= screen_nbr_imapsl || !screen_imapsl[sprite])
            return false;
        while (sprite < screen_nbr_imapsl && screen_imapsl[sprite])
            sprite++;
        if (sprite == screen_nbr_imapsl)
            return false;
    }
    return false;
}

static bool entity_valid(unsigned index)
{
    if (index < 4 || index > 0x7e || index >= ent_nbr_entdata)
        return false;

    const entdata_t *entity = &ent_entdata[index];
    if (!entity->w || !entity->h || entity->w > 32 || entity->h > 32)
        return false;
    if (index < 0x18)
        return entity->spr < sprites_nbr_sprites;

    /* Type 3 repeats a nonempty sprite sequence and stops at a move sentinel. */
    size_t sprite = entity->spr + 1u;
    size_t step = entity->sni;
    if (sprite >= ent_nbr_sprseq ||
        (ent_sprseq[sprite - 1] != 0xff &&
         ent_sprseq[sprite - 1] >= sprites_nbr_sprites) ||
        step >= ent_nbr_mvstep || ent_mvstep[step].count == 0xff)
        return false;
    while (sprite < ent_nbr_sprseq && ent_sprseq[sprite] != 0xff)
    {
        if (ent_sprseq[sprite] >= sprites_nbr_sprites)
            return false;
        sprite++;
    }
    while (step < ent_nbr_mvstep && ent_mvstep[step].count != 0xff)
        step++;
    return sprite < ent_nbr_sprseq && step < ent_nbr_mvstep;
}

static bool submap_valid(const submap_t *sub)
{
    if (sub->page >= 255 || sub->page + 1u >= tiles_nbr_banks ||
        (sub->page + 1u) * 16 > map_nbr_eflgc ||
        !map_window_valid(sub - map_submaps, 0))
        return false;

    unsigned flags = 0;
    for (unsigned i = 0; i < 16; i += 2)
        flags += map_eflg_c[sub->page * 16 + i];
    if (flags != sizeof(map_eflg))
        return false;

    size_t c = sub->connect;
    for (; c < map_nbr_connect && map_connect[c].dir != 0xff; c++)
    {
        const connect_t *link = &map_connect[c];
        if (link->dir > 1 ||
            (link->submap != 0xff && link->submap >= map_nbr_submaps))
            return false;
    }
    if (c >= map_nbr_connect)
        return false;

    size_t m = sub->mark;
    while (m < map_nbr_marks && map_marks[m].row != 0xff)
        m++;
    return m < map_nbr_marks;
}

bool xrick_resources_valid(void)
{
    /* The intro has five hard-coded map drawings, including the finale. */
    if (map_nbr_maps != 5 || screen_nbr_imapsofs != map_nbr_maps ||
        screen_nbr_imaptext != map_nbr_maps || game_color_count < 32 ||
        game_color_count > 256 || !sprites_nbr_sprites ||
        sprites_nbr_sprites > 255 || tiles_nbr_banks > 255 ||
        ent_nbr_sprseq < 8 || !screen_nbr_hiscores || screen_nbr_hiscores > 8 ||
        !tile_text(screen_pausedtxt, 25, 15) ||
        !tile_text(screen_gameovertxt, 25, 15))
        return false;

    for (size_t i = 0; i < screen_nbr_hiscores; i++)
        for (size_t j = 0; j < HISCORE_NAME_SIZE; j++)
            if (screen_highScores[i].name[j] >= 0xfe)
                return false;

    for (size_t i = 0; i < map_nbr_bnums; i++)
        if (map_bnums[i] >= map_nbr_blocks)
            return false;
    for (size_t i = 0; i < 8; i++)
        if (ent_sprseq[i] >= sprites_nbr_sprites)
            return false;
    for (size_t i = 0; i < screen_nbr_imapsl; i++)
        if (screen_imapsl[i] >= sprites_nbr_sprites)
            return false;

    for (size_t i = 0; i < map_nbr_marks; i++)
        if (map_marks[i].row != 0xff && !entity_valid(map_marks[i].ent & 0x7f))
            return false;
    for (size_t i = 0; i < map_nbr_submaps; i++)
        if (!submap_valid(&map_submaps[i]))
            return false;
    for (size_t i = 0; i < map_nbr_maps; i++)
    {
        const map_t *map = &map_maps[i];
        if (map->x >= 256 || map->y >= 320 || map->row > 255 ||
            !map_window_valid(map->submap, map->row) || !intro_valid(i))
            return false;
    }
    return true;
}
