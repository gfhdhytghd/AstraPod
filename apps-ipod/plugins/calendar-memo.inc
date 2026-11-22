/* AstraPod calendar memo adapter. GNU GPL version 2 or later. */
static void load_memo(struct shown *shown)
{
    rb->memset(day_has_memo, 0, sizeof(day_has_memo));
    rb->memset(wday_has_memo, 0, sizeof(wday_has_memo));
    memos_in_memory = 0;
    int fd = rb->open(MEMO_FILE, O_RDONLY);
    if (fd < 0)
        return;

    while (memos_in_memory < MAX_MEMOS_IN_A_MONTH)
    {
        char line[10 + MAX_CHAR_MEMO_LEN];
        off_t start = rb->lseek(fd, 0, SEEK_CUR);
        if (start < 0)
            break;
        size_t used = 0;
        bool overflow = false;
        char byte;
        ssize_t read;
        while ((read = rb->read(fd, &byte, 1)) == 1 && byte != '\n')
        {
            if (byte == '\r')
                continue;
            if (byte == '\0')
                overflow = true;
            if (used < sizeof(line) - 1)
                line[used++] = byte;
            else
                overflow = true;
        }
        if (read < 0 || (read == 0 && used == 0))
            break;
        line[used] = '\0';
        size_t digits = 0;
        while (digits < 10 && line[digits] >= '0' && line[digits] <= '9')
            digits++;
        if (digits != 10 || overflow)
            continue;

        int day = (line[0] - '0') * 10 + line[1] - '0';
        int month = (line[2] - '0') * 10 + line[3] - '0';
        int year = (line[4] - '0') * 1000 + (line[5] - '0') * 100 +
                   (line[6] - '0') * 10 + line[7] - '0';
        int wday = line[8] - '0';
        int type = line[9] - '0';
        if (day < 1 || day > 31 || month < 1 || month > 12 ||
            wday > 6 || type > 3 ||
            (type >= 2 && month != shown->mon) ||
            (type == 3 && year != shown->year))
            continue;

        off_t end = rb->lseek(fd, 0, SEEK_CUR);
        if (end < start)
            break;
        struct memo *memo = &memos[memos_in_memory++];
        memo->day = day;
        memo->month = month;
        memo->year = year;
        memo->wday = wday;
        memo->type = type;
        memo->file_pointer_start = start;
        memo->file_pointer_end = end;
        rb->strlcpy(memo->message, line + 10, sizeof(memo->message));
        if (type == 0)
            wday_has_memo[wday] = true;
        else
            day_has_memo[day] = true;
    }
    rb->close(fd);
}

static bool copy_memo_bytes(int from, int to, off_t length)
{
    char buffer[256];
    while (length > 0)
    {
        size_t size = MIN(length, (off_t)sizeof(buffer));
        if (rb->read(from, buffer, size) != (ssize_t)size ||
            rb->write(to, buffer, size) != (ssize_t)size)
            return false;
        length -= size;
    }
    return true;
}

static bool save_memo(int changed, bool new_mod, struct shown *shown)
{
    const struct memo *memo = &memos[changed];
    int input = rb->open(MEMO_FILE, O_RDONLY);
    off_t size = input >= 0 ? rb->ffilesize(input) : 0;
    bool ok = memo->file_pointer_start >= 0 &&
              memo->file_pointer_start <= memo->file_pointer_end &&
              memo->file_pointer_end <= size &&
              (input >= 0 || !rb->file_exists(MEMO_FILE));
    int output = ok ? rb->creat(TEMP_FILE, 0666) : -1;
    if (output < 0)
        ok = false;

    if (ok && input >= 0)
        ok = copy_memo_bytes(input, output, memo->file_pointer_start);
    if (ok && new_mod)
    {
        char line[11 + MAX_CHAR_MEMO_LEN];
        int length = rb->snprintf(line, sizeof(line), "%02d%02d%04d%01d%01d%s\n",
                                  memo->day, memo->month, memo->year,
                                  memo->wday, memo->type, memo->message);
        ok = length > 0 && (size_t)length < sizeof(line) &&
             rb->write(output, line, length) == length;
    }
    if (ok && input >= 0)
        ok = rb->lseek(input, memo->file_pointer_end, SEEK_SET) ==
             memo->file_pointer_end &&
             copy_memo_bytes(input, output, size - memo->file_pointer_end);
    if (input >= 0)
        rb->close(input);
    if (output >= 0)
    {
        if (ok)
            ok = rb->file_sync(output) == 0;
        if (rb->close(output) < 0)
            ok = false;
        if (ok)
            ok = rb->rename(TEMP_FILE, MEMO_FILE) == 0;
        if (!ok)
            rb->remove(TEMP_FILE);
    }

    load_memo(shown);
    if (!ok)
        rb->splash(HZ, "Could not save events");
    return ok;
}
