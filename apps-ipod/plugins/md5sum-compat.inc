/* Owned filesystem operations for MD5Sum. GPL version 2 or later. */
#include "fs_attr.h"

#define RECORD_SIZE (MD5_STRING_LENGTH + 2 + MAX_PATH + 1)

enum scan_mode { SCAN_PATH, SCAN_LIST, CHECK_LIST };
enum scan_stop { SCAN_ACTIVE, SCAN_CANCEL, SCAN_USB };

static struct {
    DIR *directory;
    int input;
    int output;
    int hashed;
    bool owns_temp;
    enum scan_stop stop;
    unsigned int files;
    long next_progress;
    char result[MAX_PATH];
    char temp[MAX_PATH];
} scan;

static unsigned char file_block[16384];
static unsigned char list_block[4096];
static char output_line[RECORD_SIZE];

static int close_file(int *fd)
{
    if (*fd < 0)
        return 0;
    int result = rb->close(*fd);
    *fd = -1;
    return result;
}

static bool close_dir(void)
{
    if (!scan.directory)
        return true;
    int result = rb->closedir(scan.directory);
    scan.directory = NULL;
    return result == 0;
}

static void discard_scan(void *unused)
{
    (void)unused;
    close_dir();
    close_file(&scan.input);
    close_file(&scan.hashed);
    close_file(&scan.output);
    if (scan.owns_temp)
    {
        rb->remove(scan.temp);
        scan.owns_temp = false;
    }
}

static bool poll_scan(void)
{
    int action = rb->get_action(CONTEXT_STD, TIMEOUT_NOBLOCK);
    if (action == ACTION_STD_CANCEL || action == ACTION_STD_MENU)
        scan.stop = SCAN_CANCEL;
    else if (rb->default_event_handler_ex(action, discard_scan, NULL) ==
             SYS_USB_CONNECTED)
        scan.stop = SCAN_USB;

    if (scan.stop != SCAN_ACTIVE)
        return false;
    if (!TIME_BEFORE(*rb->current_tick, scan.next_progress))
    {
        rb->splash_progress(0, 1, "MD5Sum - Menu to cancel\n%u files",
                            scan.files);
        scan.next_progress = *rb->current_tick + HZ / 8;
    }
    rb->yield();
    return true;
}

/* Resolve list-relative paths and normalize output aliases before hashing. */
static bool normalize_path(char *out, const char *path, const char *list)
{
    if (!*path)
        return false;

    size_t used = 0;
    if (*path != '/')
    {
        if (!list)
            return false;
        used = rb->strrchr(list, '/') - list;
        rb->memcpy(out, list, used);
    }
    while (*path)
    {
        if (*path == '/')
        {
            path++;
            continue;
        }
        const char *end = rb->strchr(path, '/');
        size_t len = end ? (size_t)(end - path) : rb->strlen(path);
        if (len == 2 && path[0] == '.' && path[1] == '.')
        {
            if (used == 0)
                return false;
            while (out[--used] != '/')
                ;
        }
        else if (!(len == 1 && path[0] == '.'))
        {
            if (used + len + 1 >= MAX_PATH)
                return false;
            out[used++] = '/';
            rb->memcpy(out + used, path, len);
            used += len;
        }
        path += len;
    }
    if (used == 0)
        out[used++] = '/';
    out[used] = '\0';
    return !rb->strchr(out, '\r') && !rb->strchr(out, '\n');
}

static bool is_output(const char *path)
{
    return !rb->strcasecmp(path, scan.result) ||
           !rb->strcasecmp(path, scan.temp);
}

static bool write_line(int length)
{
    return length >= 0 && (size_t)length < sizeof(output_line) &&
           rb->write(scan.output, output_line, length) == length;
}

static bool hash_file(const char *path, char digest[MD5_STRING_LENGTH + 1])
{
    if (!poll_scan())
        return false;
    scan.hashed = rb->open(path, O_RDONLY);
    if (scan.hashed < 0)
        return false;

    struct md5_s md5;
    InitMD5(&md5);
    off_t remaining = rb->ffilesize(scan.hashed);
    bool ok = remaining >= 0;
    while (ok)
    {
        if (!poll_scan())
        {
            ok = false;
            break;
        }
        ssize_t bytes = rb->read(scan.hashed, file_block, sizeof(file_block));
        if (bytes <= 0 || bytes > remaining)
        {
            ok = bytes == 0 && remaining == 0;
            break;
        }
        remaining -= bytes;
        AddMD5(&md5, file_block, bytes);
    }
    if (close_file(&scan.hashed) < 0 || !ok)
        return false;
    EndMD5(&md5);
    psz_md5_hash(digest, &md5);
    return true;
}

static bool write_sum(const char *path)
{
    if (is_output(path))
        return true;

    char digest[MD5_STRING_LENGTH + 1];
    if (rb->strchr(path, '\r') || rb->strchr(path, '\n') ||
        !hash_file(path, digest))
        return false;
    scan.files++;
    return write_line(rb->snprintf(output_line, sizeof(output_line),
                                  "%s  %s\n", digest, path));
}

static bool hash_path(char *path)
{
    scan.directory = rb->opendir(path);
    if (!scan.directory)
        return write_sum(path);

    /* Close a parent on descent. After returning, find the same child
     * before continuing; a missing resume entry makes the scan fail. */
    char resume[MAX_PATH] = "";
    size_t base = rb->strlen(path);
    while (poll_scan())
    {
        struct DIRENT *entry;
        struct dirinfo info;
        int status = rb->dir_next(scan.directory, &entry, &info);
        if (status < 0)
            break;

        size_t length = rb->strlen(path);
        if (status == 0)
        {
            if (*resume || !close_dir())
                break;
            if (length == base)
                return true;
            char *slash = rb->strrchr(path, '/');
            rb->strlcpy(resume, slash + 1, sizeof(resume));
            slash[slash == path ? 1 : 0] = '\0';
            scan.directory = rb->opendir(path);
            if (!scan.directory)
                break;
            continue;
        }

        if (*resume)
        {
            if (!rb->strcmp(entry->d_name, resume))
                resume[0] = '\0';
            continue;
        }

        if (info.attribute & (ATTR_LINK | ATTR_VOLUME))
            continue;
        int appended = rb->snprintf(path + length, MAX_PATH - length,
                                    "%s%s", length == 1 ? "" : "/",
                                    entry->d_name);
        if (appended < 0 || (size_t)appended >= MAX_PATH - length)
            break;
        if (info.attribute & ATTR_DIRECTORY)
        {
            if (!close_dir())
                break;
            scan.directory = rb->opendir(path);
            if (!scan.directory)
                break;
        }
        else
        {
            if (!write_sum(path))
                break;
            path[length] = '\0';
        }
    }
    return false;
}

static bool check_sum(char *line, const char *list)
{
    if (rb->strlen(line) < MD5_STRING_LENGTH + 3 ||
        line[MD5_STRING_LENGTH] != ' ' ||
        (line[MD5_STRING_LENGTH + 1] != ' ' &&
         line[MD5_STRING_LENGTH + 1] != '*'))
        return false;

    line[MD5_STRING_LENGTH] = '\0';
    for (unsigned int i = 0; i < MD5_STRING_LENGTH; i++)
        if (!rb->strchr("0123456789abcdefABCDEF", line[i]))
            return false;

    const char *name = line + MD5_STRING_LENGTH + 2;
    char path[MAX_PATH];
    if (!normalize_path(path, name, list) || is_output(path))
        return false;

    char digest[MD5_STRING_LENGTH + 1];
    bool readable = hash_file(path, digest);
    if (scan.stop != SCAN_ACTIVE)
        return false;
    const char *result = !readable ? "FAILED open or read" :
                         rb->strcasecmp(line, digest) ? "FAILED" : "OK";
    scan.files++;
    return write_line(rb->snprintf(output_line, sizeof(output_line),
                                  "%s: %s\n", name, result));
}

static bool scan_record(char *record, size_t used, const char *list,
                        enum scan_mode mode)
{
    if (used && record[used - 1] == '\r')
        used--;
    record[used] = '\0';
    if (!used)
        return true;
    if (!poll_scan())
        return false;
    if (mode == CHECK_LIST)
        return check_sum(record, list);

    char target[MAX_PATH];
    return normalize_path(target, record, list) && hash_path(target);
}

static bool scan_list(const char *path, enum scan_mode mode)
{
    scan.input = rb->open(path, O_RDONLY);
    if (scan.input < 0)
        return false;
    off_t remaining = rb->ffilesize(scan.input);
    char record[RECORD_SIZE];
    size_t used = 0;
    while (remaining >= 0 && poll_scan())
    {
        ssize_t bytes = rb->read(scan.input, list_block, sizeof(list_block));
        if (bytes <= 0)
            return bytes == 0 && remaining == 0 &&
                   scan_record(record, used, path, mode) &&
                   close_file(&scan.input) == 0;
        if (bytes > remaining)
            return false;
        remaining -= bytes;
        for (ssize_t i = 0; i < bytes; i++)
        {
            unsigned char byte = list_block[i];
            if (byte == '\n')
            {
                if (!scan_record(record, used, path, mode))
                    return false;
                used = 0;
            }
            else if (byte == '\0' || used == sizeof(record) - 1)
                return false;
            else
                record[used++] = byte;
        }
    }
    return false;
}

enum plugin_status plugin_start(const void *parameter)
{
    scan = (__typeof__(scan)){ .input = -1, .output = -1, .hashed = -1,
                               .next_progress = *rb->current_tick };
    char input[MAX_PATH];
    enum scan_mode mode = SCAN_PATH;
    int length;
    if (parameter)
    {
        if (!normalize_path(input, parameter, NULL))
            return PLUGIN_ERROR;

        const char *ext = rb->strrchr(input, '.');
        if (ext && (!rb->strcasecmp(ext, ".md5") ||
                    !rb->strcasecmp(ext, ".md5sum")))
        {
            length = rb->snprintf(scan.result, sizeof(scan.result),
                                  "%.*s.md5check", (int)(ext - input), input);
            mode = CHECK_LIST;
        }
        else
        {
            length = rb->snprintf(scan.result, sizeof(scan.result),
                                  "%s.md5sum", input);
            if (ext && !rb->strcasecmp(ext, ".md5list"))
                mode = SCAN_LIST;
        }
    }
    else
    {
        static const char *lines[] = {
            "Compute checksums for all files?",
            "Write /everything.md5sum"
        };
        const struct text_message prompt = { lines, ARRAYLEN(lines) };
        enum yesno_res answer = rb->gui_syncyesno_run(&prompt, NULL, NULL);
        if (answer != YESNO_YES)
            return answer == YESNO_USB ? PLUGIN_USB_CONNECTED : PLUGIN_OK;
        rb->strcpy(input, "/");
        length = rb->strlcpy(scan.result, "/everything.md5sum",
                            sizeof(scan.result));
    }
    if (length < 0 || (size_t)length >= sizeof(scan.result))
        return PLUGIN_ERROR;
    length = rb->snprintf(scan.temp, sizeof(scan.temp), "%s.tmp", scan.result);
    if (length < 0 || (size_t)length >= sizeof(scan.temp))
        return PLUGIN_ERROR;

    scan.output = rb->open(scan.temp, O_WRONLY | O_CREAT | O_EXCL);
    scan.owns_temp = scan.output >= 0;
    rb->splash_progress_set_delay(0);
    bool ok = scan.owns_temp &&
              (mode == SCAN_PATH ? hash_path(input) : scan_list(input, mode));
    if (ok)
    {
        int synced = rb->file_sync(scan.output);
        int closed = close_file(&scan.output);
        ok = !synced && !closed && poll_scan() &&
             rb->rename(scan.temp, scan.result) == 0;
        if (ok)
            scan.owns_temp = false;
    }
    discard_scan(NULL);

    if (scan.stop == SCAN_USB || rb->plugin_usb_taken())
        return PLUGIN_USB_CONNECTED;
    if (scan.stop == SCAN_ACTIVE)
    {
        if (ok)
            rb->splashf(2 * HZ, "Saved %u results\n%s", scan.files,
                        scan.result);
        else
            rb->splash(2 * HZ, "MD5Sum could not read or save checksums");
    }
    return ok || scan.stop == SCAN_CANCEL ? PLUGIN_OK : PLUGIN_ERROR;
}
