/* Bounded OTP1 accounts and URI import. GPL version 2 or later. */
#include <stddef.h>
#include "plugins/phase3-file-io.h"

#define OTP_OFFSET_LIMIT (14 * 3600)

_Static_assert(sizeof(struct account_t) == 328 &&
               offsetof(struct account_t, hotp_counter) == 56,
               "OTP1 record layout");

static bool save_failed;

static bool valid_account(const struct account_t *account)
{
    size_t name = 0;
    while (name < sizeof(account->name) && account->name[name])
        name++;
    if (!name || name == sizeof(account->name))
        return false;

    /* Validate the stored byte before evaluating a C bool. */
    const unsigned char *raw = (const unsigned char *)account;
    if (raw[offsetof(struct account_t, is_totp)] > 1 || account->digits < 1
        || account->digits > 9 || account->sec_len < 1
        || account->sec_len > SECRET_MAX)
        return false;

    return !account->is_totp ||
           account->totp_period > 0;
}

static int read_accts(void)
{
    int fd = rb->open(ACCT_FILE, O_RDONLY);
    if (fd < 0)
        return rb->file_exists(ACCT_FILE) ? -1 : 0;

    off_t size = rb->ffilesize(fd);
    char magic[4];
    bool ok = size >= (off_t)(sizeof(magic) + sizeof(time_offs))
              && rb->read(fd, magic, sizeof(magic)) == sizeof(magic)
              && !rb->memcmp(magic, "OTP1", sizeof(magic))
              && rb->read(fd, &time_offs, sizeof(time_offs)) == sizeof(time_offs)
              && time_offs >= -OTP_OFFSET_LIMIT && time_offs <= OTP_OFFSET_LIMIT;

    if (ok)
    {
        uintmax_t records = size - sizeof(magic) - sizeof(time_offs);
        ok = records % sizeof(*accounts) == 0
             && records / sizeof(*accounts) <= (uintmax_t)max_accts;
        if (ok)
        {
            next_slot = records / sizeof(*accounts);
            ok = rb->read(fd, accounts, records) == (ssize_t)records;
            for (int i = 0; i < next_slot && ok; i++)
                ok = valid_account(&accounts[i]);
        }
    }
    if (rb->close(fd) < 0)
        ok = false;
    return ok ? 1 : -1;
}

static bool save_accts(void)
{
    char temp[MAX_PATH];
    int fd = begin_replace(ACCT_FILE, temp);
    bool ok = false;
    if (fd >= 0)
    {
        size_t size = sizeof(*accounts) * next_slot;
        ok = rb->write(fd, "OTP1", 4) == 4
             && rb->write(fd, &time_offs, sizeof(time_offs)) == sizeof(time_offs)
             && rb->write(fd, accounts, size) == (ssize_t)size;
        ok = end_replace(fd, temp, ACCT_FILE, ok);
    }
    if (!ok && !rb->plugin_usb_taken())
        rb->splash(HZ * 2, "Accounts not saved");
    save_failed = !ok;
    return ok;
}

static int hex_digit(unsigned char ch)
{
    if (ch >= '0' && ch <= '9')
        return ch - '0';
    ch |= 0x20;
    return ch >= 'a' && ch <= 'f' ? ch - 'a' + 10 : -1;
}

static bool account_name(char *out, size_t size, const char *label)
{
    char *end = out + size - 1;
    char *start = out;
    while (*label)
    {
        unsigned char value = *label++;
        if (value == '%')
        {
            if (!label[0] || !label[1])
                return false;
            int high = hex_digit(*label++);
            int low = hex_digit(*label++);
            if (high < 0 || low < 0)
                return false;
            value = high * 16 + low;
        }
        if (value < ' ' || value == 0x7f || out == end)
            return false;
        *out++ = value;
    }
    *out = '\0';
    return out != start;
}

static bool parse_uri(struct account_t *account, char *uri)
{
    const char *prefix = "otpauth://";
    size_t prefix_size = rb->strlen(prefix);
    if (rb->strncmp(uri, prefix, prefix_size))
        return false;
    char *kind = uri + prefix_size;
    char *label = rb->strchr(kind, '/');
    if (!label)
        return false;
    *label++ = '\0';
    char *query = rb->strchr(label, '?');
    if (!query)
        return false;
    *query++ = '\0';

    rb->memset(account, 0, sizeof(*account));
    account->digits = 6;
    if (!rb->strcmp(kind, "totp"))
    {
#if CONFIG_RTC
        account->is_totp = true;
        account->totp_period = 30;
#else
        return false;
#endif
    }
    else if (rb->strcmp(kind, "hotp"))
        return false;
    if (!account_name(account->name, sizeof(account->name), label)
        || acct_exists(account->name))
        return false;

    char *state;
    for (char *field = rb->strtok_r(query, "&", &state); field;
         field = rb->strtok_r(NULL, "&", &state))
    {
        char *value = rb->strchr(field, '=');
        if (!value || value == field)
            return false;
        *value++ = '\0';
        uintmax_t number;
        if (!rb->strcmp(field, "secret"))
        {
            if (account->sec_len)
                return false;
            account->sec_len = base32_decode(account->secret, SECRET_MAX,
                                              (const uint8_t *)value);
            if (account->sec_len <= 0)
                return false;
        }
        else if (!rb->strcmp(field, "digits"))
        {
            if (!parse_unsigned(value, 9, &number) || !number)
                return false;
            account->digits = number;
        }
        else if (!rb->strcmp(field, "counter"))
        {
            if (account->is_totp || !parse_unsigned(value, UINT64_MAX, &number))
                return false;
            account->hotp_counter = number;
        }
        else if (!rb->strcmp(field, "period"))
        {
            if (!account->is_totp || !parse_unsigned(value, UINT32_MAX, &number)
                || !number)
                return false;
            account->totp_period = number;
        }
        else if (!rb->strcmp(field, "algorithm") && rb->strcasecmp(value, "SHA1"))
            return false;
    }
    return account->sec_len > 0;
}

static void add_acct_file(void)
{
    char path[MAX_PATH];
    if (rb->pick_file(path, sizeof(path), "/", "Choose OTP account file",
                      NULL, 0, false) <= 0)
        return;

    off_t bom;
    int fd = open_text(path, &bom);
    if (fd < 0)
    {
        rb->splash(HZ, "Unable to open account file");
        return;
    }

    int before = next_slot;
    int invalid = 0;
    int len = 0;
    char uri[URI_MAX];
    bool ok = true;
    while (next_slot < max_accts
           && (len = rb->read_line(fd, uri, sizeof(uri))) > 0)
    {
        if (rb->action_userabort(TIMEOUT_NOBLOCK))
        {
            ok = false;
            break;
        }
        size_t size = rb->strlen(uri);
        if ((size_t)len >= sizeof(uri) - 1 || (size_t)len > size + 2)
        {
            ok = false;
            break;
        }
        if (size && parse_uri(&accounts[next_slot], uri))
            next_slot++;
        else if (size)
            invalid++;
    }
    if (rb->close(fd) < 0 || len < 0 || !ok)
    {
        next_slot = before;
        if (!rb->plugin_usb_taken())
            rb->splash(HZ, "Account import cancelled or unreadable");
        return;
    }
    if (next_slot == before)
        rb->splash(HZ, "No valid accounts found");
    else if (save_accts())
        rb->splashf(HZ * 2, "Added %d, skipped %d", next_slot - before, invalid);
}
