/*
 * This file is part of mpv.
 *
 * mpv is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * mpv is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with mpv.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <pthread.h>
#include <errno.h>
#include <assert.h>

#include "osdep/threads.h"
#include "osdep/timer.h"
#include "stream/stream.h"
#include "stream/cache_ctrl.h"
#include "demux.h"
#include "stheader.h"

// Time in seconds to update various cached information
#define CACHE_UPDATE_TIME 1.0

// Number of packets the demuxer thread should read in advance.
#define NUM_READAHEAD_PACKETS 70

struct priv {
    pthread_t demux_thread;
    pthread_mutex_t mutex;
    pthread_cond_t wakeup;

    // --- Owned by the main thread
    struct demuxer *wrapper;

    // --- Owned by demuxer thread
    struct demuxer *demuxer;

    // --- Shared state protected by the mutex
    bool thread_request_kill;
    bool thread_request_pause; // see pause_thread()
    bool thread_paused;
    bool read_packets;
    // Used to detect when streams were added.
    int num_streams;
    // Other cached state
    double start_time;
    double time_length;
    struct stream_cache_ctrls *cache_ctrls;
};

// Called in demuxer thread, with mutex held.
static void update_infos(struct priv *p)
{
    p->start_time = demuxer_get_start_time(p->demuxer);
    p->time_length = demuxer_get_time_length(p->demuxer);
    stream_cache_ctrl_update(p->cache_ctrls, p->demuxer->stream);
}

// Called in demuxer thread, with mutex held.
static int read_packet(struct priv *p)
{
    int r = 0;

    if (!p->read_packets)
        return r;

    // Check limits; the thread shouldn't demux the whole file.
    bool enough = true;
    for (int n = 0; n < p->demuxer->num_streams; n++) {
        struct sh_stream *sh = p->demuxer->streams[n];
        if (sh->type == STREAM_VIDEO || sh->type == STREAM_AUDIO) {
            if (sh->selected) {
                size_t size = 0;
                int count = 0;
                packet_queue_add_size(sh->pq, &size, &count);
                if (count < NUM_READAHEAD_PACKETS)
                    enough = false;
            }
        }
    }
    if (enough)
        return r;

    pthread_mutex_unlock(&p->mutex);
    if (demux_fill_buffer(p->demuxer) > 0)
        r = 1;
    pthread_mutex_lock(&p->mutex);

    p->num_streams = p->demuxer->num_streams;

    pthread_cond_signal(&p->wakeup);
    return r;
}

// Cooperative pausing of the demuxer thread. This asks the demuxer thread to
// wait until we unpause it. This is used to access the demuxer directly from
// the main thread, which simplifies code because we don't have to marshal
// function calls between the threads.
//
// Basically, this is needed whenever accessing things from the playback thread
// that are also accessed by the demux thread, but not protected by a mutex.
//
// Use only for slow/rare operations.
// Usually unlocks temporarily.
static void pause_thread(struct priv *p)
{
    p->thread_request_pause = true;
    pthread_cond_signal(&p->wakeup);
    while (!p->thread_paused)
        pthread_cond_wait(&p->wakeup, &p->mutex);
}

// Undo pause_thread().
// This never unlocks (important to avoid data races).
static void resume_thread(struct priv *p)
{
    assert(p->thread_request_pause && p->thread_paused);
    p->thread_request_pause = false;
    pthread_cond_signal(&p->wakeup);
}

static void *demux_thread(void *arg)
{
    struct priv *p = arg;
    update_infos(p);
    double last = mp_time_sec();
    pthread_mutex_lock(&p->mutex);
    while (!p->thread_request_kill) {
        while (p->thread_request_pause) {
            p->thread_paused = true;
            pthread_cond_signal(&p->wakeup);
            pthread_cond_wait(&p->wakeup, &p->mutex);
        }
        p->thread_paused = false;

        pthread_mutex_unlock(&p->mutex);
        if (mp_time_sec() - last > CACHE_UPDATE_TIME) {
            update_infos(p);
            last = mp_time_sec();
        }
        pthread_mutex_lock(&p->mutex);

        if (p->thread_request_pause || p->thread_request_kill)
            continue;

        if (!read_packet(p)) {
            if (p->thread_request_pause || p->thread_request_kill)
                continue;
            // Nothing to do -> safe CPU time
            mpthread_cond_timed_wait(&p->wakeup, &p->mutex, 10.0);
        }
    }
    pthread_cond_signal(&p->wakeup);
    pthread_mutex_unlock(&p->mutex);
    mp_msg(MSGT_CACHE, MSGL_V, "Demuxer thread exiting...\n");
    return NULL;
}

// Add streams that appeared after initialization.
// Must be called only while demuxer thread is paused.
static void add_stream_headers(struct priv *p)
{
    assert(p->demuxer->num_streams >= p->wrapper->num_streams);
    for (int n = p->wrapper->num_streams; n < p->demuxer->num_streams; n++) {
        struct sh_stream *src = p->demuxer->streams[n];
        struct sh_stream *dst = new_sh_stream(p->wrapper, src->type);
        assert(src->index == dst->index);

        // The packet queue is going to be replaced with the source demuxer's.
        assert(packet_queue_is_empty(dst->pq));
        talloc_free(dst->pq);

        // Apply some very dirty tricks to get all fields copied.
        // Note that referencing memory from the src demuxer is ok, because
        // all data should be immutable until the demuxer is destroyed.
        struct sh_stream orig = *dst;
        *dst = *src;
        dst->demuxer = orig.demuxer;
    }
    p->num_streams = p->wrapper->num_streams;
}

// Note: since the real demuxer shares the packet queues with the wrapper
//       demuxer, this is called only when a stream actually runs out of
//       packets. Thus, we always have to block and read a packet directly
//       in order to keep the unthreaded demuxer semantics.
static int d_fill_buffer(demuxer_t *demuxer)
{
    struct priv *p = demuxer->priv;
    int r = 0;

    pthread_mutex_lock(&p->mutex);
    // Stop the demuxer thread from accessing p->demuxer.
    // (Note that in some situations, we get here before the demuxer thread
    //  finishes reading a packet, so that after pausing there will be a new
    //  packet, but not before pausing. In this case we'd read a second packet,
    //  which is dumb and might be not ideal for very slow streams. But at
    //  least this code is simpler, and it's not really a race condition.)
    pause_thread(p);

    if (p->num_streams != demuxer->num_streams)
        add_stream_headers(p);

    r = demux_fill_buffer(p->demuxer);

    // Let the demuxer thread read more packets after this.
    p->read_packets = true;

    resume_thread(p);
    pthread_mutex_unlock(&p->mutex);
    return r;
}

static void d_seek(demuxer_t *demuxer, float rel_seek_secs, int flags)
{
    struct priv *p = demuxer->priv;

    pthread_mutex_lock(&p->mutex);
    pause_thread(p);
    demux_seek(p->demuxer, rel_seek_secs, flags);
    resume_thread(p);
    pthread_mutex_unlock(&p->mutex);
}

static int d_control(demuxer_t *demuxer, int cmd, void *arg)
{
    struct priv *p = demuxer->priv;
    int r = DEMUXER_CTRL_NOTIMPL;

    pthread_mutex_lock(&p->mutex);

    switch (cmd) {
    case DEMUXER_CTRL_GET_TIME_LENGTH:
        *(double *)arg = p->time_length;
        r = DEMUXER_CTRL_OK;
        break;
    case DEMUXER_CTRL_GET_START_TIME:
        *(double *)arg = p->start_time;
        r = DEMUXER_CTRL_OK;
        break;
    case DEMUXER_CTRL_UPDATE_INFO:
        if (p->num_streams != demuxer->num_streams) {
            pause_thread(p);
            add_stream_headers(p);
            resume_thread(p);
        }
        r = DEMUXER_CTRL_OK;
        break;
    case DEMUXER_CTRL_FLUSH:
        pause_thread(p);
        // This avoids race conditions with the stream layer if demux_seek()
        // does stream-based seeks: if we don't set this, the thread would
        // start reading packets after flushing, and before the stream layer
        // performs the actual seek. Other reasons to flush the demuxer might
        // behave in similar ways.
        p->read_packets = false;
        resume_thread(p);
        r = DEMUXER_CTRL_OK;
        break;
    case DEMUXER_CTRL_SWITCHED_TRACKS:
        pause_thread(p);
        add_stream_headers(p);
        for (int n = 0; n < p->wrapper->num_streams; n++) {
            struct sh_stream *src = p->wrapper->streams[n];
            struct sh_stream *dst = p->demuxer->streams[n];
            demuxer_select_track(p->demuxer, dst,
                                 demuxer_stream_is_selected(p->wrapper, src));
        }
        p->demuxer->stream_autoselect = p->wrapper->stream_autoselect;
        resume_thread(p);
        r = DEMUXER_CTRL_OK;
        break;
    default:
        //  e.g. DEMUXER_CTRL_IDENTIFY_PROGRAM
        pause_thread(p);
        r = demux_control(p->demuxer, cmd, arg);
        resume_thread(p);
    }

    pthread_mutex_unlock(&p->mutex);
    return r;
}

static void d_close(demuxer_t *demuxer)
{
    struct priv *p = demuxer->priv;

    pthread_mutex_lock(&p->mutex);
    p->thread_request_kill = true;
    pthread_cond_signal(&p->wakeup);
    pthread_mutex_unlock(&p->mutex);
    pthread_join(p->demux_thread, NULL);

    // Demuxers are never supposed to close the stream, so just disable it.
    demuxer->stream->priv = NULL;
}

static int s_fill_buffer(struct stream *cache, char *buffer, int max_len)
{
    mp_msg(MSGT_CACHE, MSGL_ERR, "Trying to read from wrapper stream.\n");
    return -1;
}

static int s_seek(stream_t *cache, int64_t pos)
{
    mp_msg(MSGT_CACHE, MSGL_ERR, "Trying to seek in wrapper stream.\n");
    return 0;
}

// This needs to be provided for "special" operations, e.g. DVD and BD.
static int s_control(stream_t *s, int cmd, void *arg)
{
    struct priv *p = s->priv;

    if (!p)
        return STREAM_ERROR;

    pthread_mutex_lock(&p->mutex);
    int r = stream_cache_ctrl_get(p->cache_ctrls, cmd, arg);
    if (r == STREAM_ERROR) {
        pause_thread(p);
        r = stream_control(p->demuxer->stream, cmd, arg);
        resume_thread(p);
    }
    pthread_mutex_unlock(&p->mutex);
    return r;
}

static void s_close(stream_t *s)
{
    s->priv = NULL;
}

static const demuxer_desc_t demuxer_desc_thread_wrapper = {
    .name = "thread_wrapper",
    .desc = "Demuxer threading wrapper",
    .open = NULL,
    .fill_buffer = d_fill_buffer,
    .seek = d_seek,
    .control = d_control,
    .close = d_close,
};

struct demuxer *demux_create_thread_wrapper(struct demuxer *demuxer)
{
    struct demuxer *wrapper = talloc(NULL, struct demuxer);
    struct priv *p = talloc_zero(wrapper, struct priv);

    // Dirty trick to get most fields copied. Most fields are supposed to be
    // immutable after initialization, so this works.
    *wrapper = *demuxer;

    wrapper->priv = p;
    wrapper->desc = &demuxer_desc_thread_wrapper;

    // The list of streams is not immutable.
    wrapper->streams = NULL;
    wrapper->num_streams = 0;

    wrapper->stream = stream_create_wrapper(demuxer->stream);
    wrapper->stream->uncached_stream = NULL; // don't let it recursively free
    wrapper->stream->fill_buffer = s_fill_buffer;
    wrapper->stream->seek = s_seek;
    wrapper->stream->control = s_control;
    wrapper->stream->close = s_close;
    wrapper->stream->priv = p;

    p->wrapper = wrapper;
    p->demuxer = demuxer;
    p->cache_ctrls = talloc_zero(p, struct stream_cache_ctrls);

    add_stream_headers(p);
    update_infos(p);

    pthread_mutex_init(&p->mutex, NULL);
    pthread_cond_init(&p->wakeup, NULL);

    if (pthread_create(&p->demux_thread, NULL, demux_thread, p) != 0) {
        mp_msg(MSGT_CACHE, MSGL_ERR, "Starting cache process/thread failed: %s.\n",
               strerror(errno));
        return NULL;
    }
    return wrapper;
}
