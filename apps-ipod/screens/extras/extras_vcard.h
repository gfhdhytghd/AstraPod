/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * vCard 2.1 / 3.0 / 4.0 scanner, loader, and lossless atomic editor for
 * /Contacts .vcf files and /Contacts/AstraPod/<UID>.vcf.
 ****************************************************************************/
#ifndef PODBOX_SCREENS_EXTRAS_VCARD_H
#define PODBOX_SCREENS_EXTRAS_VCARD_H

#include <stdbool.h>
#include <stdint.h>
#include "extras_store.h"

#define VCARD_DIR           "/Contacts"
#define VCARD_SUBDIR        "AstraPod"
#define VCARD_MAX_PHONES    8
#define VCARD_MAX_EMAILS    6
#define VCARD_MAX_ADDRS     4

enum vcard_version {
    VCARD_VER_30 = 0,
    VCARD_VER_40,
    VCARD_VER_21
};

enum vcard_phone_type {
    VCARD_TEL_MOBILE = 0,
    VCARD_TEL_HOME,
    VCARD_TEL_WORK,
    VCARD_TEL_MAIN,
    VCARD_TEL_FAX,
    VCARD_TEL_PAGER,
    VCARD_TEL_OTHER,
    VCARD_TEL_TYPE_COUNT
};

enum vcard_kind_type {
    VCARD_KIND_HOME = 0,
    VCARD_KIND_WORK,
    VCARD_KIND_OTHER,
    VCARD_KIND_TYPE_COUNT
};

struct vcard_phone {
    uint32_t source_offset;          /* 0 for a newly added row */
    enum vcard_phone_type type;
    char value[64];
};

struct vcard_email {
    uint32_t source_offset;          /* 0 for a newly added row */
    enum vcard_kind_type type;
    char value[96];
};

struct vcard_addr {
    uint32_t source_offset;          /* 0 for a newly added row */
    enum vcard_kind_type type;
    char pobox[48];
    char ext[48];
    char street[96];
    char city[64];
    char region[48];
    char postal[32];
    char country[64];
};

struct vcard_contact {
    char path[MAX_PATH];
    uint32_t card_start;
    uint32_t card_end;               /* 0 = new contact */
    bool is_existing;
    enum vcard_version version;
    char uid[128];

    char first[96];
    char last[96];
    char middle[64];
    char prefix[32];
    char suffix[32];
    char fn[160];
    char org[128];

    int phone_count;
    struct vcard_phone phones[VCARD_MAX_PHONES];
    int email_count;
    struct vcard_email emails[VCARD_MAX_EMAILS];
    int addr_count;
    struct vcard_addr addrs[VCARD_MAX_ADDRS];

    bool has_bday;
    struct extras_date bday;
    char note[512];

    /* Snapshots for lossless preservation of unmodified structured props. */
    char orig_first[96], orig_last[96];
    char orig_org[128], orig_note[512];
    uint32_t source_phones[VCARD_MAX_PHONES];
    uint32_t source_emails[VCARD_MAX_EMAILS];
    uint32_t source_addrs[VCARD_MAX_ADDRS];
    bool orig_has_bday;
    struct extras_date orig_bday;
};

/* Check whether any .vcf exists in `dir` or one-level subdirectories. */
bool vcard_any_in(const char *dir);

/* Shared by the on-disk index and the editor. */
bool vcard_read_text(char *line, size_t capacity, char **name, char **value);
void vcard_split_parts(char *value, char **parts, int count);

void vcard_init_new(struct vcard_contact *c);
/* Load one VCARD starting at `card_start` in `path`. Sets `c->card_end`. */
bool vcard_load(const char *path, uint32_t card_start,
                struct vcard_contact *c);
/* Save new or edited contact atomically. New contacts go to
 * `<contacts_dir>/AstraPod/<UID>.vcf`. */
bool vcard_save(const char *contacts_dir, struct vcard_contact *c);
/* Delete only the VCARD in [card_start, card_end). Removes the file if no
 * other VCARD remains in it. */
bool vcard_delete(const char *path, uint32_t card_start, uint32_t card_end);

/* Format a structured address into stock's multi-line display representation. */
void vcard_format_addr(const struct vcard_addr *a, char *out, size_t size);

#endif
