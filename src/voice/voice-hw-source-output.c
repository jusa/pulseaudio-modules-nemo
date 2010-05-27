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

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <pulsecore/namereg.h>
#include <pulse/rtclock.h>

#include "module-voice-userdata.h"
#include "voice-hw-source-output.h"
#include "voice-aep-ear-ref.h"
#include "voice-util.h"
#include "voice-voip-source.h"
#include "pa-optimized.h"
#include "voice-convert.h"
#include "memory.h"

#include "module-voice-api.h"
#include "voice-hooks.h"

static pa_bool_t voice_uplink_feed(struct userdata *u, pa_memchunk *chunk) {
    pa_memchunk ichunk;
    pa_assert(u);
    pa_assert(u->aep_fragment_size == chunk->length);

    if (pa_memblockq_push(u->ul_memblockq, chunk) < 0) {
	pa_log("%s %d: Failed to push %d byte chunk into memblockq (len %d).",
	       __FILE__, __LINE__, chunk->length,
	       pa_memblockq_get_length(u->ul_memblockq));
    }

    if (util_memblockq_to_chunk(u->core->mempool, u->ul_memblockq, &ichunk, u->voice_ul_fragment_size)) {
	if (pa_memblockq_get_length(u->ul_memblockq) != 0)
	    pa_log("%s %d: AEP processed UL left over %d", __FILE__, __LINE__,
		   pa_memblockq_get_length(u->ul_memblockq));

	if (PA_SOURCE_IS_OPENED(u->voip_source->thread_info.state))
	    pa_source_post(u->voip_source, &ichunk);
	pa_memblock_unref(ichunk.memblock);
	return TRUE;
    }
    else
	return FALSE;
}

static
pa_bool_t voice_voip_source_process(struct userdata *u, pa_memchunk *chunk) {
    pa_bool_t ul_frame_sent = FALSE;

    pa_assert(u);
    pa_assert(chunk->length == u->aep_fragment_size);

    if (u->voip_source->thread_info.soft_muted ||
        pa_cvolume_is_muted(&u->voip_source->thread_info.soft_volume) ||
        pa_memblock_is_silence(chunk->memblock)) {
        pa_memblock_unref(chunk->memblock);
        pa_silence_memchunk_get(&u->core->silence_cache,
                                u->core->mempool,
                                chunk,
                                &u->aep_sample_spec,
                                chunk->length);
        voice_aep_ear_ref_ul_drop_log(u, VOICE_PERIOD_AEP_USECS);
    } else {
        aep_uplink params;
        pa_memchunk rchunk;

        voice_aep_ear_ref_ul(u, &rchunk);

        params.chunk = chunk;
        params.rchunk = &rchunk;

        pa_hook_fire(u->hooks[HOOK_AEP_UPLINK], &params);

        pa_memblock_unref(rchunk.memblock);
    }

    ul_frame_sent = voice_uplink_feed(u, chunk);

    return ul_frame_sent;
}

static
void voice_uplink_timing_check(struct userdata *u, pa_usec_t now,
                               pa_bool_t ul_frame_sent) {
    int64_t to_deadline = u->ul_deadline - now;

    if (to_deadline < u->ul_timing_advance) {
        pa_usec_t forward_usecs = (pa_usec_t)
            ((((u->ul_timing_advance-to_deadline)/VOICE_PERIOD_CMT_USECS)+1)*VOICE_PERIOD_CMT_USECS);

        pa_log_debug("Deadline already missed by %lld usec (%lld < %lld + %d) forwarding %lld usecs",
                     -to_deadline + u->ul_timing_advance, u->ul_deadline, now,
                     u->ul_timing_advance, forward_usecs);
        u->ul_deadline += forward_usecs;
        to_deadline = u->ul_deadline - now;
        pa_log_debug("New deadline %lld", u->ul_deadline);
    }

    pa_log_debug("Time to next deadline %lld usecs (%d)", to_deadline, u->ul_timing_advance);
    if ((int)to_deadline < VOICE_PERIOD_MASTER_USECS + u->ul_timing_advance) {
        if (!ul_frame_sent) {
            // Flush all that we have from buffers, so we should be in time on next round
            size_t drop = pa_memblockq_get_length(u->ul_memblockq);
            pa_memblockq_drop(u->ul_memblockq, drop);
            pa_log_debug("Dropped %d bytes (%lld usec) from ul_memblockq", drop,
                         pa_bytes_to_usec_round_up((uint64_t)drop, &u->aep_sample_spec));
            drop = pa_memblockq_get_length(u->hw_source_memblockq);
            pa_memblockq_drop(u->hw_source_memblockq, drop);
            pa_log_debug("Dropped %d bytes (%lld usec) from hw_source_memblockq", drop,
                         pa_bytes_to_usec_round_up((uint64_t)drop, &u->hw_mono_sample_spec));
            voice_aep_ear_ref_ul_drop_log(u, pa_bytes_to_usec_round_up(
                                              (uint64_t)drop, &u->hw_mono_sample_spec));
        }
        else {
            pa_log_debug("Timing is correct: Frame sent at %lld and deadline at %lld",
                         now, u->ul_deadline);
        }
        u->ul_deadline = 0;
    }
}

/*** hw_source_output callbacks ***/

/* Called from I/O thread context */
static void hw_source_output_push_cb(pa_source_output *o, const pa_memchunk *new_chunk) {
    struct userdata *u;
    pa_memchunk chunk;
    pa_bool_t ul_frame_sent = FALSE;
    pa_usec_t now = pa_rtclock_now();

    pa_assert(o);
    pa_assert_se(u = o->userdata);

#ifdef SOURCE_TIMING_DEBUG_ON
    static struct timeval tv_last = { 0, 0 };
    struct timeval tv_new;
    pa_rtclock_get(&tv_new);
    pa_usec_t ched_period = pa_timeval_diff(&tv_last, &tv_new);
    tv_last = tv_new;
#endif

    if (pa_memblockq_push(u->hw_source_memblockq, new_chunk) < 0) {
        pa_log("Failed to push %d byte chunk into memblockq (len %d).",
               new_chunk->length, pa_memblockq_get_length(u->hw_source_memblockq));
        return;
    }

    while (util_memblockq_to_chunk(u->core->mempool, u->hw_source_memblockq, &chunk, u->aep_hw_fragment_size)) {

        if (voice_voip_source_active_iothread(u)) {
            /* This branch is taken when call is active */
            pa_memchunk chunkmono, chunk8k;

            switch (u->active_mic_channel) {
            default:
                pa_log_error("u->active_mic_channel value invalid");
                pa_assert_not_reached();

            case MIC_BOTH:
                pa_optimized_downmix_to_mono(&chunk, &chunkmono);
                break;

            case MIC_CH0:
                pa_optimized_take_channel(&chunk, &chunkmono, 0);
                break;

            case MIC_CH1:
                pa_optimized_take_channel(&chunk, &chunkmono, 1);
                break;
            }

            /* RMC used only with ECI headsets that have one mic */
            pa_hook_fire(u->hooks[HOOK_RMC_MONO], &chunkmono);

            voice_convert_run_48_to_8(u, u->hw_source_to_aep_resampler, &chunkmono, &chunk8k);
            pa_memblock_unref(chunkmono.memblock);
            pa_hook_fire(u->hooks[HOOK_NARROWBAND_MIC_EQ_MONO], &chunk8k);

            ul_frame_sent = voice_voip_source_process(u, &chunk8k);
            pa_memblock_unref(chunk8k.memblock);

        } else {
            /* This branch is taken when call is not active e.g. when source.voice.raw is used */

            /* used to store mono audio temporarily */
            pa_memchunk ochunk_ch0;
            pa_memchunk ochunk_ch1;

            /* NOTE: Raw source samples are not run trough any EQ if AEP is running. */
            pa_optimized_deinterleave_stereo_to_mono(&chunk, &ochunk_ch0, &ochunk_ch1);
            pa_memblock_unref(chunk.memblock);

            pa_hook_fire(u->hooks[HOOK_WIDEBAND_MIC_EQ_MONO], &ochunk_ch0);
            pa_hook_fire(u->hooks[HOOK_WIDEBAND_MIC_EQ_MONO], &ochunk_ch1);

            pa_optimized_interleave_stereo(&ochunk_ch0, &ochunk_ch1, &chunk);

            pa_memblock_unref(ochunk_ch0.memblock);
            pa_memblock_unref(ochunk_ch1.memblock);
        }

        if (PA_SOURCE_IS_OPENED(u->raw_source->thread_info.state)) {
            pa_source_post(u->raw_source, &chunk);
        }

        pa_memblock_unref(chunk.memblock);
    }

    if (u->ul_deadline)
        voice_uplink_timing_check(u, now, ul_frame_sent);

#ifdef SOURCE_TIMING_DEBUG_ON
    pa_rtclock_get(&tv_new);
    pa_usec_t process_delay = pa_timeval_diff(&tv_last, &tv_new);
    printf("%d,%d ", (int)ched_period, (int)process_delay);
#endif
}

/* Called from I/O thread context */
static void hw_source_output_push_cb_8k_mono(pa_source_output *o, const pa_memchunk *new_chunk) {
    struct userdata *u;
    pa_memchunk chunk;
    pa_bool_t ul_frame_sent = FALSE;
    pa_usec_t now = pa_rtclock_now();

    pa_assert(o);
    pa_assert_se(u = o->userdata);

#ifdef SOURCE_TIMING_DEBUG_ON
    static struct timeval tv_last = { 0, 0 };
    struct timeval tv_new;
    pa_rtclock_get(&tv_new);
    pa_usec_t ched_period = pa_timeval_diff(&tv_last, &tv_new);
    tv_last = tv_new;
#endif

    if (pa_memblockq_push(u->hw_source_memblockq, new_chunk) < 0) {
        pa_log("Failed to push %d byte chunk into memblockq (len %d).",
               new_chunk->length, pa_memblockq_get_length(u->hw_source_memblockq));
        return;
    }

    /* Assume 8kHz mono */
    while (util_memblockq_to_chunk(u->core->mempool, u->hw_source_memblockq, &chunk, u->aep_fragment_size)) {
        if (voice_voip_source_active_iothread(u)) {
            ul_frame_sent = voice_voip_source_process(u, &chunk);
        }

        if (PA_SOURCE_IS_OPENED(u->raw_source->thread_info.state)) {
	    pa_memchunk ochunk;
	    voice_convert_run_8_to_48(u, u->hw8khz_source_to_raw_source_resampler, &chunk, &ochunk);
            /* TODO: Mabe we should fire narrowband mic eq here */
            pa_source_post(u->raw_source, &ochunk);
	    pa_memblock_unref(ochunk.memblock);
        }
        pa_memblock_unref(chunk.memblock);
    }

    voice_uplink_timing_check(u, now, ul_frame_sent);

#ifdef SOURCE_TIMING_DEBUG_ON
    pa_rtclock_get(&tv_new);
    pa_usec_t process_delay = pa_timeval_diff(&tv_last, &tv_new);
    printf("%d,%d ", (int)ched_period, (int)process_delay);
#endif
}

/* Called from I/O thread context */
static int hw_source_output_process_msg(pa_msgobject *mo, int code, void *userdata, int64_t offset, pa_memchunk *chunk) {
    pa_source_output *o = PA_SOURCE_OUTPUT(mo);
    struct userdata *u;
    pa_source_output_assert_ref(o);

    pa_assert_se(u = o->userdata);

    switch (code) {

        case PA_SOURCE_OUTPUT_MESSAGE_GET_LATENCY: {
            pa_usec_t *r = userdata;

            r[0] += pa_bytes_to_usec(pa_memblockq_get_length(u->hw_source_memblockq), &o->sample_spec);

            break;
        }
    }

    return pa_source_output_process_msg(mo, code, userdata, offset, chunk);
}

static
size_t hw_source_output_convert_bytes(pa_source_output *o, pa_source *s, size_t nbytes)
{
    return (nbytes/pa_frame_size(&o->thread_info.sample_spec))*pa_frame_size(&s->sample_spec);
}

/* Called from I/O thread context */
static void hw_source_output_process_rewind_cb(pa_source_output *o, size_t nbytes) {
    struct userdata *u;

    pa_source_output_assert_ref(o);
    pa_assert_se(u = o->userdata);

    if (!PA_SOURCE_OUTPUT_IS_LINKED(o->thread_info.state))
        return;

    if (u->raw_source && PA_SOURCE_IS_OPENED(u->raw_source->thread_info.state)) {
	size_t amount = hw_source_output_convert_bytes(o, u->raw_source, nbytes);
	pa_source_process_rewind(u->raw_source, amount);
    }

    if (u->voip_source && PA_SOURCE_IS_OPENED(u->voip_source->thread_info.state)) {
	size_t amount = hw_source_output_convert_bytes(o, u->voip_source, nbytes);
	pa_source_process_rewind(u->voip_source, amount);
    }
}

/* Called from I/O thread context */
static void hw_source_output_update_max_rewind_cb(pa_source_output *o, size_t nbytes) {
    struct userdata *u;

    pa_source_output_assert_ref(o);
    pa_assert_se(u = o->userdata);

    if (!PA_SOURCE_OUTPUT_IS_LINKED(o->thread_info.state))
        return;

    if (u->raw_source && PA_SOURCE_IS_LINKED(u->raw_source->thread_info.state))
        pa_source_set_max_rewind_within_thread(u->raw_source, hw_source_output_convert_bytes(o, u->raw_source, nbytes));

    if (u->voip_source && PA_SOURCE_IS_LINKED(u->voip_source->thread_info.state))
        pa_source_set_max_rewind_within_thread(u->voip_source, hw_source_output_convert_bytes(o, u->voip_source, nbytes));
}

/* Called from I/O thread context */
static void hw_source_output_update_source_latency_range_cb(pa_source_output *o) {
    struct userdata *u;

    pa_source_output_assert_ref(o);
    pa_assert_se(u = o->userdata);

    if (u->raw_source && PA_SOURCE_IS_LINKED(u->raw_source->thread_info.state))
	pa_source_set_latency_range_within_thread(u->raw_source, o->source->thread_info.min_latency, o->source->thread_info.max_latency);

    if (u->voip_source && PA_SOURCE_IS_LINKED(u->voip_source->thread_info.state))
	pa_source_set_latency_range_within_thread(u->voip_source, o->source->thread_info.min_latency, o->source->thread_info.max_latency);
}

/* Called from I/O thread context */
static void hw_source_output_update_source_fixed_latency_cb(pa_source_output *o) {
    struct userdata *u;

    pa_source_output_assert_ref(o);
    pa_assert_se(u = o->userdata);

    if (u->raw_source && PA_SOURCE_IS_LINKED(u->raw_source->thread_info.state))
        pa_source_set_fixed_latency_within_thread(u->raw_source, o->source->thread_info.fixed_latency);

    if (u->voip_source && PA_SOURCE_IS_LINKED(u->voip_source->thread_info.state))
        pa_source_set_fixed_latency_within_thread(u->voip_source, o->source->thread_info.fixed_latency);
}

/* Called from I/O thread context */
static void hw_source_output_detach_cb(pa_source_output *o) {
    struct userdata *u;

    pa_source_output_assert_ref(o);
    pa_assert_se(u = o->userdata);

    if (!u->raw_source || !PA_SOURCE_IS_LINKED(u->raw_source->thread_info.state))
        return;

    u->master_source = NULL;

    /* there is no aep-source-output to drive voip_source */
    pa_source_detach_within_thread(u->voip_source);
    pa_source_set_asyncmsgq(u->voip_source, NULL);
    pa_source_set_rtpoll(u->voip_source, NULL);
    voice_source_outputs_may_move(u->voip_source, FALSE);

    pa_source_detach_within_thread(u->raw_source);
    pa_source_set_asyncmsgq(u->raw_source, NULL);
    pa_source_set_rtpoll(u->raw_source, NULL);
    voice_source_outputs_may_move(u->raw_source, FALSE);

    pa_log_debug("Detach called");
}

/* Called from I/O thread context */
static void hw_source_output_attach_slave_source(struct userdata *u, pa_source *source, pa_source *to_source) {
    pa_assert(u);
    pa_assert(to_source);

    if (source && PA_SOURCE_IS_LINKED(source->thread_info.state)) {
        pa_source_set_rtpoll(source, to_source->thread_info.rtpoll);
        pa_source_attach_within_thread(source);
        pa_source_set_latency_range_within_thread(source, to_source->thread_info.min_latency,
                                                  to_source->thread_info.max_latency);
        pa_source_set_fixed_latency_within_thread(source, to_source->thread_info.fixed_latency);
    }
}

/* Called from I/O thread context */
static void hw_source_output_attach_cb(pa_source_output *o) {
    struct userdata *u;

    pa_source_output_assert_ref(o);
    pa_assert_se(u = o->userdata);

    u->master_source = o->source;

    pa_log_debug("Attach called, new master %p %s", (void*)u->master_source, u->master_source->name);
    hw_source_output_attach_slave_source(u, u->raw_source, o->source);
    hw_source_output_attach_slave_source(u, u->voip_source, o->source);
}

/* Called from main thread context */
static void hw_source_output_update_slave_source(struct userdata *u, pa_source *source, pa_source *new_master) {
    pa_proplist *p;
    pa_source_output *o;
    uint32_t idx;
    pa_assert(u);
    pa_assert(source);
    pa_assert(new_master);

    pa_source_update_flags(source, PA_SOURCE_LATENCY|PA_SOURCE_DYNAMIC_LATENCY, new_master->flags);
    pa_source_set_asyncmsgq(source, new_master->asyncmsgq);

    p = pa_proplist_new();
    pa_proplist_setf(p, PA_PROP_DEVICE_DESCRIPTION, "%s source connected to %s", source->name, new_master->name);
    pa_proplist_sets(p, PA_PROP_DEVICE_MASTER_DEVICE, new_master->name);
    pa_source_update_proplist(source, PA_UPDATE_REPLACE, p);
    pa_proplist_free(p);

    /* Call moving callbacks of slave sources's source-outputs. */
    PA_IDXSET_FOREACH(o, source->outputs, idx)
        if (o->moving)
            o->moving(o, source);
}

/* Called from main thread context */
static void hw_source_output_moving_cb(pa_source_output *o, pa_source *dest) {
    struct userdata *u;

    pa_source_output_assert_ref(o);
    pa_assert_se(u = o->userdata);

    pa_log_debug("Source output moving to %s", dest ? dest->name : "(null)");
    hw_source_output_update_slave_source(u, u->raw_source, dest);
    hw_source_output_update_slave_source(u, u->voip_source, dest);

    if (!dest)
        return; /* The source output is going to be killed, don't do anything. */

    u->master_source = dest;

    if ((o->sample_spec.rate == VOICE_SAMPLE_RATE_AEP_HZ &&
	 dest->sample_spec.rate != VOICE_SAMPLE_RATE_AEP_HZ) ||
	(o->sample_spec.rate != VOICE_SAMPLE_RATE_AEP_HZ &&
	 dest->sample_spec.rate == VOICE_SAMPLE_RATE_AEP_HZ)) {
	pa_log_info("Reinitialize due to samplerate change %d->%d.",
                    o->sample_spec.rate, dest->sample_spec.rate);
        pa_log_debug("New source format %s", pa_sample_format_to_string(dest->sample_spec.format)) ;
        pa_log_debug("New source rate %d", dest->sample_spec.rate);
        pa_log_debug("New source channels %d", dest->sample_spec.channels);

        voice_reinit_hw_source_output(u);
    }
}

/* Called from main thread context */
static void hw_source_output_kill_cb(pa_source_output* o) {
    struct userdata *u;

    pa_source_output_assert_ref(o);
    pa_assert_se(u = o->userdata);

    pa_source_unlink(u->voip_source);
    pa_source_unlink(u->raw_source);
    pa_source_output_unlink(o);

    pa_source_unref(u->voip_source);
    pa_source_unref(u->raw_source);
    u->raw_source = NULL;
    u->voip_source = NULL;

    pa_source_output_unref(o);
    u->hw_source_output = NULL;
}

static pa_bool_t hw_source_output_may_move_to_cb(pa_source_output *o, pa_source *dest) {
    struct userdata *u;

    pa_source_output_assert_ref(o);
    pa_assert_se(u = o->userdata);

    if (u->master_source == NULL)
        return TRUE;

    return ((u->master_source != dest) && (u->master_source->asyncmsgq != dest->asyncmsgq));
}

static pa_hook_result_t hw_source_output_move_fail_cb(pa_core *c, pa_source_output *o, struct userdata *u) {
    const char *master_source;
    pa_source *s = NULL;

    pa_assert(u);
    pa_source_output_assert_ref(o);

    if (o != u->hw_source_output)
        return PA_HOOK_OK;

    master_source = pa_modargs_get_value(u->modargs, "master_source", NULL);

    if (!master_source
        || !(s = pa_namereg_get(u->core, master_source, PA_NAMEREG_SOURCE))) {

        pa_log("Master source \"%s\" not found", master_source);
        return PA_HOOK_OK;
    }

    if (pa_source_output_finish_move(o, s, TRUE) >= 0)
        return PA_HOOK_STOP;

    pa_log("Failed to fallback on \"%s\".", master_source);

    /* ok fallback on destroying the hw_source_ouput (voice module should probably be unloaded) */
    return PA_HOOK_OK;
}

/* Currently only 48kHz stereo and 8kHz mono are supported. */
static pa_source_output *voice_hw_source_output_new(struct userdata *u, pa_source_output_flags_t flags)
{
    pa_source_output_new_data so_data;
    pa_source_output *new_source_output;
    char t[256];

    pa_assert(u);
    pa_assert(u->master_source);
    ENTER();

    snprintf(t, sizeof(t), VOICE_MASTER_SOURCE_OUTPUT_NAME);

    pa_source_output_new_data_init(&so_data);
    so_data.flags = flags;
    so_data.driver = __FILE__;
    so_data.module = u->master_source->module;
    so_data.source = u->master_source;
    pa_proplist_sets(so_data.proplist, PA_PROP_MEDIA_NAME, t);
    pa_proplist_sets(so_data.proplist, PA_PROP_APPLICATION_NAME, t); /* this is the default value used by PA modules */
    if (u->master_source->sample_spec.rate == VOICE_SAMPLE_RATE_AEP_HZ) {
	pa_source_output_new_data_set_sample_spec(&so_data, &u->aep_sample_spec);
	pa_source_output_new_data_set_channel_map(&so_data, &u->aep_channel_map);
    }
    else {
	pa_source_output_new_data_set_sample_spec(&so_data, &u->hw_sample_spec);
	pa_source_output_new_data_set_channel_map(&so_data, &u->stereo_map);
    }

    pa_source_output_new(&new_source_output, u->master_source->core, &so_data);
    pa_source_output_new_data_done(&so_data);

    if (!new_source_output) {
        pa_log("Failed to create source output to source \"%s\".", u->master_source->name);
        return NULL;
    }

    if (u->master_source->sample_spec.rate == VOICE_SAMPLE_RATE_AEP_HZ)
        new_source_output->push = hw_source_output_push_cb_8k_mono;
    else
        /* mono */
        new_source_output->push = hw_source_output_push_cb;
    new_source_output->parent.process_msg = hw_source_output_process_msg;
    new_source_output->process_rewind = hw_source_output_process_rewind_cb;
    new_source_output->update_max_rewind = hw_source_output_update_max_rewind_cb;
    new_source_output->update_source_latency_range = hw_source_output_update_source_latency_range_cb;
    new_source_output->update_source_fixed_latency = hw_source_output_update_source_fixed_latency_cb;
    new_source_output->attach = hw_source_output_attach_cb;
    new_source_output->detach = hw_source_output_detach_cb;
    new_source_output->moving = hw_source_output_moving_cb;
    new_source_output->kill = hw_source_output_kill_cb;
    new_source_output->may_move_to = hw_source_output_may_move_to_cb;
    new_source_output->userdata = u;

    return new_source_output;
}

int voice_init_hw_source_output(struct userdata *u) {
    pa_assert(u);

    u->hw_source_output = voice_hw_source_output_new(u, 0);
    pa_return_val_if_fail (u->hw_source_output, -1);

    u->hw_source_output_move_fail_slot = pa_hook_connect(&u->core->hooks[PA_CORE_HOOK_SOURCE_OUTPUT_MOVE_FAIL], PA_HOOK_EARLY, (pa_hook_cb_t) hw_source_output_move_fail_cb, u);

    return 0;
}

struct voice_hw_source_output_reinit_defered {
    struct userdata *u;
    pa_defer_event *defer;
};

static void voice_hw_source_output_reinit_defer_cb(pa_mainloop_api *m, pa_defer_event *de, void *userdata) {
    struct voice_hw_source_output_reinit_defered *d;
    pa_source_output *new_so, *old_so;
    struct userdata *u;
    pa_bool_t start_uncorked;

    pa_assert_se(d = userdata);
    pa_assert_se(u = d->u);
    pa_assert_se(old_so = u->hw_source_output);

    m->defer_enable(d->defer, 0);
    pa_xfree(d);
    d = NULL;

    start_uncorked = PA_SOURCE_IS_OPENED(pa_source_get_state(u->raw_source)) ||
        PA_SOURCE_IS_OPENED(pa_source_get_state(u->voip_source)) ||
        pa_source_output_get_state(old_so) != PA_SOURCE_OUTPUT_CORKED;
    pa_log("HWSO START UNCORKED: %d", start_uncorked);

    new_so = voice_hw_source_output_new(u, start_uncorked ? 0 : PA_SOURCE_OUTPUT_START_CORKED);
    pa_return_if_fail (new_so);

    pa_source_output_cork(old_so, TRUE);

    pa_log_debug("reinitialize hw source-output %s %p", u->master_source->name, (void*)new_so);

    u->hw_source_output = new_so;
    pa_source_output_put(u->hw_source_output);

    pa_log_debug("Detaching the old sink input %p", (void*)old_so);

    old_so->detach = NULL;
    pa_source_output_unlink(old_so);
    pa_source_output_unref(old_so);

    voice_aep_ear_ref_loop_reset(u);
}

void voice_reinit_hw_source_output(struct userdata *u) {
    struct voice_hw_source_output_reinit_defered *d;
    pa_assert(u);

    d = pa_xnew0(struct voice_hw_source_output_reinit_defered, 1); /* in theory should be tracked if pulseaudio exit before defer called (could be added to userdata, but would need to be queued */
    d->u = u;
    d->defer = u->core->mainloop->defer_new(u->core->mainloop, voice_hw_source_output_reinit_defer_cb, d);
}
