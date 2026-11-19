/* Resident Cider client; owns no audio or app_buffer. GPL version 2+. */
#ifndef CIDER_H
#define CIDER_H
#include "config.h"
#include "screens/playback/cider_model.h"
#include "screens/playback/cider_store.h"
#ifdef HAVE_USB_PODLINK
#define CIDER_COVER_DIM 128
#define CIDER_COVER_SCRATCH_BYTES (CIDER_COVER_DIM * CIDER_COVER_DIM * sizeof(uint16_t))
void cider_store_stats(struct cider_store_stats *out);
void cider_store_build(bool enable);
bool cider_store_building(void);
bool cider_art_local(const char *key, const char *tier);
void cider_read_ahead(const char *const *keys, unsigned int count, const char *tier);
unsigned int cider_read_ahead_count(unsigned int speed, unsigned int visible);
void cider_init(void);
bool cider_connected(void);
bool cider_available(void);
bool cider_active(void);
const char *cider_remote_name(void);
void cider_remote_home(void);
bool cider_play_pending(void);
void cider_remote_source(void);
void cider_source_poll(void);
void cider_snapshot(struct cider_state *state);
/* Snapshot the advertised artwork independently of the decoded WPS cover. */
void cider_current_art(char key[65]);
/* Schedule a missing current cover at idle. True means its request failed. */
bool cider_request_playing_art(const char *key);
bool cider_lyrics_matches(const char *id, uint32_t generation, uint32_t library);
/* Small transport state for callers on tight stacks (UI, skin, status). */
struct cider_status { bool available, muted, loading; int rate, volume, shuffle, repeat; };
void cider_status(struct cider_status *status);
void cider_spectrum_wps(bool visible);
bool cider_spectrum_levels(unsigned char *levels);
void cider_loading_drawn(bool loading);
void cider_control(const char *command, int value);
int cider_mode_get(int mode);
void cider_mode_set(int mode, int value);
void cider_prefetch_art(const char *key, const char *tier);
void cider_warm_art(const char *const *keys, unsigned int count, const char *tier);
bool cider_palette_supported(void);
void cider_request_art(const char *key, const char *tier);
void cider_cancel_art(void);
void cider_pin_art(const char *key);
void cider_unpin_art(void);
void cider_retain_art(const char *const *keys, unsigned int count,
                      const char *tier);
unsigned int cider_art_revision(void);
bool cider_has_art(const char *key, const char *tier);
void cider_request_xs(const char *const *keys, unsigned int count);
bool cider_copy_art(const char *key, const char *tier, void *pixels,
                     unsigned int size);
bool cider_play(const char *route);
/* Play a song of the list 'context' (a page route) at row 'index' */
bool cider_play_in(const char *route, const char *context, int index);
int cider_play_status(void);
struct cider_row;
uint32_t cider_panel_revision(void);
bool cider_panel_album(int index, struct cider_row *row, uint32_t *revision);
void cider_track_page_open(const char *route);
int cider_track_page_total(bool *stale);
bool cider_track_page_row(int index, struct cider_row *row);
void cider_local_play(void);
void cider_page_open(const char *route);
int cider_page_total(void);
bool cider_page_stale(void);
bool cider_page_row(int index, struct cider_row *row);
#else
static inline bool cider_play_pending(void) { return false; }
static inline const char *cider_remote_name(void) { return "Cider"; }
static inline void cider_remote_source(void) {}
static inline void cider_source_poll(void) {}
static inline bool cider_active(void) { return false; }
static inline bool cider_connected(void) { return false; }
static inline bool cider_available(void) { return false; }
static inline void cider_snapshot(struct cider_state *s)
{ *s = (struct cider_state){0}; }
struct cider_status { bool available, muted, loading; int rate, volume, shuffle, repeat; };
static inline void cider_status(struct cider_status *s)
{ *s = (struct cider_status){0}; }
static inline void cider_spectrum_wps(bool visible) { (void)visible; }
static inline bool cider_spectrum_levels(unsigned char *levels)
{ (void)levels; return false; }
static inline void cider_loading_drawn(bool loading) { (void)loading; }
static inline void cider_local_play(void) { }
static inline void cider_control(const char *c, int v) { (void)c; (void)v; }
static inline int cider_mode_get(int m) { (void)m; return 0; }
static inline void cider_mode_set(int m, int v) { (void)m; (void)v; }
#endif
#endif
