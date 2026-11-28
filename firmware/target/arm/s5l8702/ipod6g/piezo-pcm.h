/***************************************************************************
 * GNU General Public License (version 2+)
 ****************************************************************************/
#ifndef PIEZO_PCM_H
#define PIEZO_PCM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

void piezo_pcm_enable(bool enabled);
void piezo_pcm_set_volume(int tenth_db);
void piezo_pcm_set_rate(unsigned int hz);
bool piezo_pcm_selected(void);
bool piezo_pcm_active(void);
void piezo_pcm_lock(bool locked);
void piezo_pcm_start(const void *addr, size_t size);
void piezo_pcm_stop(void);
void piezo_pcm_irq(void);
#ifdef HAVE_HW_H264
uint32_t piezo_pcm_played_bytes(void);
#endif

/* The target's built-in sink resumes I2S after the last piezo buffer. */
void piezo_pcm_resume_codec(const void *addr, size_t size);

#endif
