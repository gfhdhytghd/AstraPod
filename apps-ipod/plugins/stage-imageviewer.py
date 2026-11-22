#!/usr/bin/env python3
"""Stage the stock viewer and the callable image decoder entry."""
from pathlib import Path
import sys

path, out = map(Path, sys.argv[1:])
code = path.read_text()

def cut(start, end, body=""):
    global code
    index = code.index(start)
    code = code[:index] + body + code[code.index(end, index):]

if path.name == "rb_glue.h":
    code = code[:code.index("#if defined(DEBUG)")] + "#undef printf\n#define printf(...) ((void)0)\n"
elif path.name == "yuv2rgb.h":
    code = code.replace("enum dither_modes", "enum image_viewer_dither_modes")
elif path.name == "imageviewer.h":
    code = code.replace('#include "plugin.h"', '#include "debug.h"\n#include "plugin.h"')
    cut("#if (CONFIG_PLATFORM & PLATFORM_NATIVE) && defined(HAVE_DISK_STORAGE)",
        "#if PLUGIN_BUFFER_SIZE", "")
    cut("/* image decoder header */", "#endif /* _IMAGE_VIEWER_H */", """\
#ifdef IMGDEC
#include "draw/resize.h"
#define format_native (*rb->image_native_format)
#undef COMP
extern const struct imgdec_api *iv;
extern const struct image_decoder image_decoder;

#define IMGDEC_HEADER \\
    const struct imgdec_api *iv; \\
    enum plugin_status plugin_start(const void *parameter) \\
    { \\
        struct rock_image_decoder_request *request = \\
            (struct rock_image_decoder_request *)parameter; \\
        if (!request || request->version != ROCK_IMAGE_DECODER_ABI || \\
            request->parent_api_size != sizeof(struct imgdec_api) || \\
            request->decoder_size != sizeof(struct image_decoder) || \\
            !request->parent_api) \\
            return PLUGIN_ERROR; \\
        iv = request->parent_api; \\
        request->decoder = &image_decoder; \\
        return PLUGIN_OK; \\
    }
#endif

""")
elif path.name == "image_decoder.c":
    code = code[:code.index("static void *decoder_handle")]
    code += """\
const struct image_decoder *load_decoder(struct loader_info *loader_info)
{
    if ((unsigned)loader_info->type >= MAX_IMAGE_TYPES)
        return NULL;

    release_decoder();
    struct rock_image_decoder_request request = {
        .version = ROCK_IMAGE_DECODER_ABI,
        .parent_api_size = sizeof(struct imgdec_api),
        .decoder_size = sizeof(struct image_decoder),
        .parent_api = loader_info->iv,
    };
    if (!rb->image_decoder_open(decoder_names[loader_info->type], &request))
    {
        rb->splash(HZ, "Image decoder unavailable");
        return NULL;
    }
    return request.decoder;
}

void release_decoder(void)
{
    rb->image_decoder_close();
}
"""
elif path.name == "imageviewer.c":
    code = code.replace('"imageviewer.cfg"', '"astrapod-imageviewer.cfg"')
    cut("static void get_pic_list", "static int change_filename", """\
#define IMAGE_SIBLINGS 512
static char *siblings[IMAGE_SIBLINGS];

static void get_pic_list(void)
{
    file_pt = siblings;
    char *leaf = rb->strrchr(np_file, '/');
    if (!leaf)
    {
        file_pt[entries++] = np_file;
        curfile = 0;
        return;
    }
    char directory[MAX_PATH];
    rb->memcpy(directory, np_file, leaf - np_file);
    directory[leaf - np_file] = 0;
    DIR *dir = rb->opendir(*directory ? directory : "/");
    if (dir)
    {
        for (unsigned int scanned = 0; scanned < 4096 && entries < IMAGE_SIBLINGS; ++scanned)
        {
            struct dirent *entry = rb->readdir(dir);
            if (!entry)
                break;
            if (rb->dir_get_info(dir, entry).attribute & ATTR_DIRECTORY)
                continue;
            const char *ext = rb->strrchr(entry->d_name, '.');
            if (!ext || (rb->strcasecmp(ext, ".bmp") && rb->strcasecmp(ext, ".jpg") &&
                rb->strcasecmp(ext, ".jpeg") && rb->strcasecmp(ext, ".jpe") &&
                rb->strcasecmp(ext, ".png") && rb->strcasecmp(ext, ".ppm") &&
                rb->strcasecmp(ext, ".gif")))
                continue;
            size_t bytes = rb->strlen(entry->d_name) + 1;
            if (bytes > buf_size)
                break;
            if (!rb->strcmp(entry->d_name, leaf + 1))
                curfile = entries;
            file_pt[entries++] = (char *)buf;
            rb->memcpy(buf, entry->d_name, bytes);
            buf += bytes;
            buf_size -= bytes;
        }
        rb->closedir(dir);
    }
    if (curfile < 0)
    {
        file_pt[0] = np_file;
        entries = 1;
        curfile = 0;
    }
    /* The pixel buffers use word-aligned RGB data. */
    size_t pad = -(uintptr_t)buf & 3;
    if (pad <= buf_size)
    {
        buf += pad;
        buf_size -= pad;
    }
    else
        buf_size = 0;
}

""")
    cut("#ifdef USE_PLUG_BUF\nstatic int ask_and_get_audio_buffer",
        "#endif /* USE_PLUG_BUF */" if "#endif /* USE_PLUG_BUF */" in code else "#endif /* USE_PLUG_BUF*/")
    code = code.replace("#endif /* USE_PLUG_BUF */\n", "", 1)
    code = code.replace("""#ifdef USE_PLUG_BUF
        if(iv_api.plug_buf)
        {
            return ask_and_get_audio_buffer(filename);
        }
        else
#endif
""", "")
    code = code.replace("""#ifdef USE_PLUG_BUF
        if (iv_api.plug_buf)
        {
            return ask_and_get_audio_buffer(filename);
        }
        else
#endif
""", "")
    cut("static bool find_album_art", "/******************** Plugin entry point", "")
    cut("    bool is_album_art = false;", "    /* should be ok to just load settings", """\
    if (!parameter)
    {
        if (rb->pick_file(np_file, sizeof(np_file), "/", "Choose image",
                          ".bmp,.jpg,.jpeg,.jpe,.png,.ppm,.gif", 0, false) <= 0)
            return rb->plugin_usb_taken() ? PLUGIN_USB_CONNECTED : PLUGIN_OK;
    }
    else if (rb->strlcpy(np_file, parameter, sizeof(np_file)) >= sizeof(np_file))
        return PLUGIN_ERROR;

    if (np_file[0] != '/')
        return PLUGIN_ERROR;
    status = get_image_type(np_file, false);
    if (status == IMAGE_UNKNOWN)
    {
        rb->splash(HZ, "Unsupported image");
        return PLUGIN_ERROR;
    }
    buf = rb->plugin_get_buffer(&buf_size);
    if (!buf || buf_size < MIN_MEM)
        return PLUGIN_ERROR;
    get_pic_list();
    decoder_buf = buf;
    decoder_buf_size = buf_size;

""")
    code = code.replace("""            if(!is_album_art)
            {
                /* suppress warning while running slideshow */
                status = get_image_type(np_file, iv_api.running_slideshow);
            }""", "            status = get_image_type(np_file, iv_api.running_slideshow);")

out.write_text(code)
