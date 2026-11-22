/* Checked levelsets and save files. GNU GPL version 2 or later. */
static bool level_char(char ch)
{
    return ch && rb->strchr(" #$.*@+-%_", ch);
}

/* Check a literal row or a packed row sequence before the game decodes it. */
static bool level_row(const char *line, int *rows, int *width, int *players)
{
    for (size_t i = 0; line[i]; i++)
    {
        if (line[i] == '|')
        {
            if (*width < 1 || ++*rows >= ROWS || !line[i + 1])
                return false;
            *width = 0;
            continue;
        }
        int run = 1;
        if (line[i] >= '1' && line[i] <= '9')
        {
            run = line[i++] - '0';
            if (line[i] >= '0' && line[i] <= '9')
                run = run * 10 + line[i++] - '0';
        }
        if (!level_char(line[i]) || run > COLS - *width)
            return false;
        if (line[i] == '@' || line[i] == '+')
        {
            if (run != 1 || *players != 0)
                return false;
            (*players)++;
        }
        *width += run;
    }
    return *width > 0 && ++*rows <= ROWS;
}

static bool read_levels(void)
{
    int fd = rb->open(buffered_boards.filename, O_RDONLY);
    if (fd < 0)
        return false;
    off_t length = rb->ffilesize(fd);
    if (length < 0 || (uint64_t)length > 4u * MAX_LEVEL_DATA)
    {
        rb->close(fd);
        return false;
    }

    size_t used = 0;
    int rows = 0, players = 0, levels = 0;
    bool ok = true;

    buffered_boards.index[0] = 0;
    for (;;)
    {
        int n = rb->read_line(fd, buf, sizeof(buf));
        size_t len = rb->strlen(buf);
        if (n < 0 || len >= sizeof(buf) - 1 || (size_t)n > len + 2)
        {
            ok = false;
            break;
        }
        if (!n || !len)
        {
            if (rows)
            {
                if (rows < 3 || players != 1 || levels >= MAX_LEVELS)
                {
                    ok = false;
                    break;
                }
                buffered_boards.index[++levels] = used;
                rows = players = 0;
            }
            if (!n)
                break;
            continue;
        }

        /* Comments between levels may carry titles or author credits. */
        if (!level_char(buf[0]) && (buf[0] < '1' || buf[0] > '9') &&
            !rows)
            continue;
        /* Permit an indented comment before or after a board. */
        size_t first = 0;
        while (buf[first] == ' ')
            first++;
        if (buf[first] == ';')
            continue;

        int width = 0;
        if (len >= sizeof(buffered_boards.data) - used ||
            !level_row(buf, &rows, &width, &players))
        {
            ok = false;
            break;
        }
        rb->memcpy(buffered_boards.data + used, buf, len);
        used += len;
        buffered_boards.data[used++] = '\n';
    }
    if (rb->close(fd) < 0)
        ok = false;

    if (!ok || !levels)
    {
        rb->splash(HZ * 2, "Invalid or oversized Sokoban levelset");
        return false;
    }
    current_info.max_level = levels;
    return true;
}

static bool valid_sokoban_path(const char *path)
{
    return path[0] == '/' && !rb->strchr(path, '\n') &&
           !rb->strchr(path, '\r');
}

static bool save(char *filename, bool solution)
{
    char temp[MAX_PATH];
    int len = rb->snprintf(temp, sizeof(temp), "%s.tmp", filename);
    if (!valid_sokoban_path(filename) || len <= 0 || len >= (int)sizeof(temp) ||
        undo_info.count >= MAX_UNDOS)
    {
        rb->splash(HZ * 2, "Invalid path or move history too long");
        return false;
    }

#if defined(IPOD_6G)
    if (solution && !rb->strncmp(filename, SOKOBAN_SAVE_FOLDER "/",
                                  sizeof(SOKOBAN_SAVE_FOLDER)))
        rb->mkdir(SOKOBAN_SAVE_FOLDER);
#endif
    int fd = rb->open(temp, O_WRONLY | O_CREAT | O_EXCL, 0666);
    if (fd < 0)
    {
        rb->splash(HZ * 2, "Cannot open save file");
        return false;
    }
    bool ok = rb->fdprintf(fd, "Sokoban:%c:%d:%d\n%s\n",
                           solution ? 'S' : 'P', current_info.level.index + 1,
                           undo_info.current, buffered_boards.filename) > 0 &&
              rb->write(fd, undo_info.history, undo_info.max) == undo_info.max &&
              rb->file_sync(fd) == 0;
    if (rb->close(fd) < 0)
        ok = false;
    if (ok && rb->rename(temp, filename) == 0)
    {
        rb->reload_directory();
        return true;
    }
    rb->remove(temp);
    rb->splash(HZ * 2, "Cannot save progress or solution");
    return false;
}

static int read_sokoban_number(const char **field, int limit)
{
    const char *p = *field;
    if (*p < '0' || *p > '9')
        return -1;

    int value = 0;
    do
    {
        int digit = *p++ - '0';
        if (value > limit / 10 || value * 10 + digit > limit)
            return -1;
        value = value * 10 + digit;
    } while (*p >= '0' && *p <= '9');
    *field = p;
    return value;
}

static bool read_sokoban_save(int fd, bool *solution, int *applied, int *length)
{
    if (rb->strncmp(buf, "Sokoban:", 8) ||
        (buf[8] != 'P' && buf[8] != 'S') || buf[9] != ':')
        return false;

    *solution = buf[8] == 'S';
    const char *field = &buf[10];
    int level = read_sokoban_number(&field, MAX_LEVELS);
    if (level <= 0 || *field++ != ':')
        return false;
    int moves = read_sokoban_number(&field, MAX_UNDOS - 1);
    if (moves < 0 || *field)
        return false;

    int len = rb->read_line(fd, buffered_boards.filename,
                           sizeof(buffered_boards.filename));
    if (len <= 0 || rb->strlen(buffered_boards.filename) >= MAX_PATH - 1 ||
        !valid_sokoban_path(buffered_boards.filename))
        return false;

    /* The history is all remaining bytes and contains only move commands. */
    ssize_t count = rb->read(fd, undo_info.history, sizeof(undo_info.history));
    if (count > 0 && undo_info.history[count - 1] == '\n')
        count--;
    if (count > 0 && undo_info.history[count - 1] == '\r')
        count--;
    if (count < moves || count >= MAX_UNDOS)
        return false;
    for (int i = 0; i < count; i++)
    {
        char ch = undo_info.history[i];
        if (!ch || !rb->strchr("lrudLRUD", ch))
            return false;
    }
    char tail;
    if (rb->read(fd, &tail, 1) != 0)
        return false;

    current_info.level.index = level - 1;
    *applied = moves;
    *length = count;
    undo_info.max = count;
    return true;
}
