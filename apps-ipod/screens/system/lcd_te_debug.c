/***************************************************************************
 * GNU General Public License (version 2+)
 ****************************************************************************/

#include <stdarg.h>
#include <stdio.h>
#include "config.h"
#include "file.h"
#include "kernel.h"
#include "rbpaths.h"
#include "lcd_te_debug.h"
#include "lcd-te-s5l8702.h"
#include "widgets/list.h"
#include "widgets/splash.h"

#define LCD_TE_LOG ROCKBOX_DIR "/lcd-te-diag.txt"
#define LCD_TE_LOG_TMP LCD_TE_LOG ".tmp"
#define LCD_TE_PREVIEW_WIDTH 80
#define LCD_TE_PREVIEW_LINES \
    (SIMPLELIST_MAX_LINES * SIMPLELIST_MAX_LINELENGTH \
     / (LCD_TE_PREVIEW_WIDTH + 1) - 2)

static int report_fd;
static bool report_ok;
static bool preview_limited;

static int report_line(const char *fmt, ...)
{
    char line[256];
    va_list ap;
    va_start(ap, fmt);
    int len = vsnprintf(line, sizeof(line) - 1, fmt, ap);
    va_end(ap);

    if (len < 0 || len >= (int)sizeof(line) - 2)
    {
        report_ok = false;
        return len;
    }

    if (simplelist_get_line_count() < LCD_TE_PREVIEW_LINES)
        simplelist_addline("%.*s", LCD_TE_PREVIEW_WIDTH, line);
    else
        preview_limited = true;

    if (len > LCD_TE_PREVIEW_WIDTH)
        preview_limited = true;

    if (report_ok)
    {
        line[len] = '\n';
        if (write(report_fd, line, len + 1) != len + 1)
            report_ok = false;
    }
    return len;
}

bool lcd_te_debug_screen(void)
{
    if (!lcd_te_supported())
    {
        splash(HZ, "Requires LCD type 1");
        return false;
    }

    splash(0, "Sampling LCD...");

    struct simplelist_info info;
    char status[48];
    simplelist_info_init(&info, "LCD TE diagnostics", 0, NULL);
    simplelist_reset_lines();
    simplelist_setline(status);
    info.scroll_all = true;

    report_fd = open(LCD_TE_LOG_TMP, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    report_ok = report_fd >= 0;
    preview_limited = false;
    bool sampled = lcd_te_run_diagnostic(report_line);
    if (report_fd >= 0 && close(report_fd) < 0)
        report_ok = false;

    bool saved = sampled && report_ok &&
                 rename(LCD_TE_LOG_TMP, LCD_TE_LOG) == 0;
    if (!saved && report_fd >= 0)
        remove(LCD_TE_LOG_TMP);

    if (!sampled)
        snprintf(status, sizeof(status), "LCD unavailable; retry");
    else if (saved)
        snprintf(status, sizeof(status), "Saved lcd-te-diag.txt");
    else
        snprintf(status, sizeof(status), "Could not save lcd-te-diag.txt");

    if (preview_limited)
        simplelist_addline("%s", saved ? "More detail in lcd-te-diag.txt" :
                           "Screen preview limited");

    return simplelist_show_list(&info);
}

const char *lcd_te_debug_sync_state(void)
{
    if (!lcd_te_supported())
        return "n/a";
    return lcd_te_enabled() ? "on" : "off";
}

bool lcd_te_debug_toggle(void)
{
    if (!lcd_te_supported())
        splash(HZ, "Requires LCD type 1");
    else
    {
        bool enable = !lcd_te_enabled();
        lcd_te_set_enabled(enable);
        if (enable != lcd_te_enabled())
            splash(HZ, "No usable LCD sync source");
    }
    return false;
}
