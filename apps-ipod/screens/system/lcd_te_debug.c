/***************************************************************************
 * GNU General Public License (version 2+)
 ****************************************************************************/

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "config.h"
#include "file.h"
#include "kernel.h"
#include "rbpaths.h"
#include "lcd_te_debug.h"
#include "lcd-s5l8702.h"
#include "lcd-te-s5l8702.h"
#include "ui/pace.h"
#include "widgets/list.h"
#include "widgets/splash.h"

#define LCD_TE_LOG ROCKBOX_DIR "/lcd-te-diag.txt"
#define LCD_TE_LOG_TMP LCD_TE_LOG ".tmp"
#define LCD_TE_PREVIEW_WIDTH 80
#define LCD_TE_PREVIEW_LINES 26

static const unsigned int timings[] = {LCD_PHTIME_CLASSIC_DEFAULT};

static int report_fd;
static bool report_ok;
static bool report_preview;
static struct {
    char status[48];
    char rows[LCD_TE_PREVIEW_LINES][LCD_TE_PREVIEW_WIDTH + 1];
    unsigned int count;
    bool limited;
} preview;

bool lcd_te_debug_valid_timing(unsigned int timing)
{
    for (unsigned int i = 0; i < ARRAYLEN(timings); ++i)
        if (timings[i] == timing)
            return true;
    return false;
}

static int report_line(const char *fmt, ...)
{
    char line[256];
    va_list ap;
    va_start(ap, fmt);
    int len = vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);

    if (len < 0 || len >= (int)sizeof(line) - 1)
    {
        report_ok = false;
        return -1;
    }

    if (report_preview)
    {
        if (preview.count < LCD_TE_PREVIEW_LINES)
        {
            int n = MIN(len, LCD_TE_PREVIEW_WIDTH);
            char *row = preview.rows[preview.count++];
            memcpy(row, line, n);
            row[n] = '\0';
        }
        else
            preview.limited = true;
        if (len > LCD_TE_PREVIEW_WIDTH)
            preview.limited = true;
    }

    if (report_ok)
    {
        line[len] = '\n';
        if (write(report_fd, line, len + 1) != len + 1)
            report_ok = false;
    }
    return len;
}

static void report_pacing(void)
{
    struct ui_pace_status status;
    ui_pace_get_status(&status);
    unsigned int millihz = (1000000000u + status.period / 2) / status.period;
    report_line("UI pacing=%s | period=%lu us %u.%03u Hz (%s)",
                status.enabled ? "on" : "off", (unsigned long)status.period,
                millihz / 1000, millihz % 1000,
                !status.enabled ? "authored 60 fps" :
                status.measured ? "measured TE" : "nominal TE");
    if (!status.divisor)
    {
        report_line("Last animation: none");
        return;
    }
    report_line("Last animation: %s rate | shown=%u missed=%u",
                status.divisor == 1 ? "full" : "half",
                status.shown, status.missed);
    report_line("Wake offset=%lu us%s | cost=%lu us | DMA max=%lu us",
                (unsigned long)status.offset,
                status.phased ? "" : " (phase unknown)",
                (unsigned long)status.cost, (unsigned long)status.dma);
    if (status.async)
        report_line("LCD service=%lu us", (unsigned long)status.busy);
}

static int save_report(bool with_preview, bool survey)
{
    if (!lcd_te_supported())
        return ENOTSUP;

    report_preview = with_preview;
    report_fd = open(LCD_TE_LOG_TMP, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    report_ok = report_fd >= 0;
    report_pacing();
    bool sampled = lcd_te_run_diagnostic(report_line, survey);
    if (report_fd >= 0 && close(report_fd) < 0)
        report_ok = false;

    if (sampled && report_ok && rename(LCD_TE_LOG_TMP, LCD_TE_LOG) == 0)
        return 0;
    if (report_fd >= 0)
        remove(LCD_TE_LOG_TMP);
    return sampled ? EIO : EBUSY;
}

int lcd_te_debug_save_report(bool survey)
{
    return save_report(false, survey);
}

static const char *preview_name(int item, void *data,
                                char *buffer, size_t size)
{
    (void)data;
    (void)buffer;
    (void)size;
    if (item == 0)
        return preview.status;
    if ((unsigned int)item <= preview.count)
        return preview.rows[item - 1];
    return "Screen preview limited";
}

bool lcd_te_debug_screen(void)
{
    if (!lcd_te_supported())
    {
        splash(HZ, "Requires LCD type 1");
        return false;
    }

    splash(0, "Sampling LCD...");
    preview.count = 0;
    preview.limited = false;
    int error = save_report(true, false);
    snprintf(preview.status, sizeof(preview.status), "%s",
             error == EBUSY ? "LCD unavailable; retry" :
             error ? "Could not save lcd-te-diag.txt" :
                     "Saved lcd-te-diag.txt");

    struct simplelist_info info;
    simplelist_info_init(&info, "LCD TE diagnostics",
                         1 + preview.count + preview.limited, NULL);
    info.get_name = preview_name;
    info.scroll_all = true;
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

bool lcd_te_debug_toggle_pacing(void)
{
    ui_te_pacing = !ui_te_pacing;
    return false;
}

static const char *timing_name(int item, void *data,
                               char *buffer, size_t size)
{
    (void)data;
    unsigned int timing = timings[item];
    snprintf(buffer, size, "0x%02x%s", timing,
             timing == lcd_te_bus_timing() ? " (active)" : "");
    return buffer;
}

bool lcd_te_debug_timing(void)
{
    if (!lcd_te_supported())
    {
        splash(HZ, "Requires LCD type 1");
        return false;
    }

    struct simplelist_info info;
    simplelist_info_init(&info, "LCD bus timing", ARRAYLEN(timings), NULL);
    info.get_name = timing_name;
    for (unsigned int i = 0; i < ARRAYLEN(timings); ++i)
        if (timings[i] == lcd_te_bus_timing())
            info.selection = i;

    if (simplelist_show_list(&info))
        return true;
    if (info.selection >= 0 &&
        !lcd_te_set_bus_timing(timings[info.selection]))
        splash(HZ, "LCD bus busy; retry");
    return false;
}
