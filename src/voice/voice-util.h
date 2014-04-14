/*
 * Copyright (C) 2010 Nokia Corporation.
 *
 * Contact: Maemo MMF Audio <mmf-audio@projects.maemo.org>
 *          or Jyri Sarha <jyri.sarha@nokia.com>
 *
 * These PulseAudio Modules are free software; you can redistribute
 * it and/or modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation
 * version 2.1 of the License.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301
 * USA.
 */
#ifndef voice_util_h
#define voice_util_h

#include "module-voice-userdata.h"
#include "parameter-hook.h"

#define ONDEBUG_TOKENS(a)

#define ENTER() pa_log_debug("%d: %s() called", __LINE__, __FUNCTION__)

/* TODO: Change ear ref loop to use pa_usec_t and get rid off these */
#define VOICE_TIMEVAL_INVALIDATE(TVal) ((TVal)->tv_usec = -1, (TVal)->tv_sec = 0)
#define VOICE_TIMEVAL_IS_VALID(TVal) ((bool) ((TVal)->tv_usec >= 0))

#define VOICE_MEMCHUNK_POOL_SIZE 128
typedef struct voice_memchunk_pool {
    pa_memchunk chunk;
    pa_atomic_ptr_t next;
} voice_memchunk_pool;

void voice_memchunk_pool_load(struct userdata *u);
void voice_memchunk_pool_unload(struct userdata *u);

static inline
pa_memchunk *voice_memchunk_pool_get(struct userdata *u) {
    voice_memchunk_pool *mp;
    do {
        mp = (voice_memchunk_pool *) pa_atomic_ptr_load(&u->memchunk_pool);
    } while (mp != NULL &&
             !pa_atomic_ptr_cmpxchg(&u->memchunk_pool, mp, pa_atomic_ptr_load(&mp->next)));
    if (mp != NULL)
        return &mp->chunk;
    pa_log_warn("voice_memchunk_pool empty, all %d slots allocated", VOICE_MEMCHUNK_POOL_SIZE);
    return NULL;
}

static inline
void voice_memchunk_pool_free(struct userdata *u, pa_memchunk *chunk) {
    voice_memchunk_pool *mp, *mp_new = (voice_memchunk_pool *)chunk;
    pa_memchunk_reset(chunk);
    do {
        mp = (voice_memchunk_pool *) pa_atomic_ptr_load(&u->memchunk_pool);
        pa_atomic_ptr_store(&mp_new->next, mp);
    } while (!pa_atomic_ptr_cmpxchg(&u->memchunk_pool, mp, mp_new));
}

void voice_clear_up(struct userdata *u);

int voice_source_set_state(pa_source *s, pa_source *other, pa_source_state_t state);

int voice_sink_set_state(pa_sink *s, pa_sink *other, pa_sink_state_t state);

pa_usec_t voice_source_get_requested_latency(pa_source *s, pa_source *other);

pa_usec_t voice_sink_get_requested_latency(pa_sink *s, pa_sink *other);

void voice_sink_inputs_may_move(pa_sink *s, bool move);
void voice_source_outputs_may_move(pa_source *s, bool move);

pa_sink *voice_get_original_master_sink(struct userdata *u);
pa_source *voice_get_original_master_source(struct userdata *u);

void voice_sink_proplist_update(struct userdata *u, pa_sink *s);
pa_hook_result_t voice_parameter_cb(pa_core *c, meego_parameter_update_args *ua, struct userdata *u);
pa_hook_result_t alsa_parameter_cb(pa_core *c, meego_parameter_update_args *ua, struct userdata *u);
pa_hook_result_t aep_parameter_cb(pa_core *c, meego_parameter_update_args *ua, struct userdata *u);
/* BEGIN OF AEP-SIDETONE SPAGETHI */
void voice_update_aep_volume(int16_t aep_step);
void voice_set_aep_runtime_switch(const char *aep_runtime_src);
void voice_shutdown_aep(void);
/* END OF AEP-SIDETONE SPAGETHI */

size_t voice_convert_nbytes(size_t nbytes, const pa_sample_spec *from_ss, const pa_sample_spec *to_ss) PA_GCC_PURE;

// For debugging...
void voice_append_chunk_to_file(struct userdata *u, const char *file_name, pa_memchunk *chunk);

#endif // voice_util_h
