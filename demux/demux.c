/*
 * DEMUXER v2.5
 *
 * This file is part of MPlayer.
 *
 * MPlayer is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * MPlayer is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with MPlayer; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */
#define DEMUX_PRIV(x) x

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>

#include <sys/types.h>
#include <sys/stat.h>

#include "config.h"
#include "mpvcore/options.h"
#include "mpvcore/av_common.h"
#include "talloc.h"
#include "mpvcore/mp_msg.h"

#include "stream/stream.h"
#include "demux.h"
#include "stheader.h"
#include "mf.h"

#include "audio/format.h"

#include <libavcodec/avcodec.h>

#if MP_INPUT_BUFFER_PADDING_SIZE < FF_INPUT_BUFFER_PADDING_SIZE
#error MP_INPUT_BUFFER_PADDING_SIZE is too small!
#endif

// Demuxer list
extern const struct demuxer_desc demuxer_desc_edl;
extern const struct demuxer_desc demuxer_desc_cue;
extern const demuxer_desc_t demuxer_desc_rawaudio;
extern const demuxer_desc_t demuxer_desc_rawvideo;
extern const demuxer_desc_t demuxer_desc_tv;
extern const demuxer_desc_t demuxer_desc_mf;
extern const demuxer_desc_t demuxer_desc_matroska;
extern const demuxer_desc_t demuxer_desc_lavf;
extern const demuxer_desc_t demuxer_desc_libass;
extern const demuxer_desc_t demuxer_desc_subreader;
extern const demuxer_desc_t demuxer_desc_playlist;

/* Please do not add any new demuxers here. If you want to implement a new
 * demuxer, add it to libavformat, except for wrappers around external
 * libraries and demuxers requiring binary support. */

const demuxer_desc_t *const demuxer_list[] = {
    &demuxer_desc_edl,
    &demuxer_desc_cue,
    &demuxer_desc_rawaudio,
    &demuxer_desc_rawvideo,
#if HAVE_TV
    &demuxer_desc_tv,
#endif
#if HAVE_LIBASS
    &demuxer_desc_libass,
#endif
    &demuxer_desc_matroska,
    &demuxer_desc_lavf,
    &demuxer_desc_mf,
    &demuxer_desc_playlist,
    // Pretty aggressive, so should be last.
    &demuxer_desc_subreader,
    /* Please do not add any new demuxers here. If you want to implement a new
     * demuxer, add it to libavformat, except for wrappers around external
     * libraries and demuxers requiring binary support. */
    NULL
};


static void add_stream_chapters(struct demuxer *demuxer);

static void packet_destroy(void *ptr)
{
    struct demux_packet *dp = ptr;
    talloc_free(dp->avpacket);
    free(dp->allocation);
}

static struct demux_packet *create_packet(size_t len)
{
    if (len > 1000000000) {
        mp_msg(MSGT_DEMUXER, MSGL_FATAL, "Attempt to allocate demux packet "
               "over 1 GB!\n");
        abort();
    }
    struct demux_packet *dp = talloc(NULL, struct demux_packet);
    talloc_set_destructor(dp, packet_destroy);
    *dp = (struct demux_packet) {
        .len = len,
        .pts = MP_NOPTS_VALUE,
        .duration = -1,
        .stream_pts = MP_NOPTS_VALUE,
        .pos = -1,
        .stream = -1,
    };
    return dp;
}

struct demux_packet *new_demux_packet(size_t len)
{
    struct demux_packet *dp = create_packet(len);
    dp->buffer = malloc(len + MP_INPUT_BUFFER_PADDING_SIZE);
    if (!dp->buffer) {
        mp_msg(MSGT_DEMUXER, MSGL_FATAL, "Memory allocation failure!\n");
        abort();
    }
    memset(dp->buffer + len, 0, MP_INPUT_BUFFER_PADDING_SIZE);
    dp->allocation = dp->buffer;
    return dp;
}

// data must already have suitable padding, and does not copy the data
struct demux_packet *new_demux_packet_fromdata(void *data, size_t len)
{
    struct demux_packet *dp = create_packet(len);
    dp->buffer = data;
    return dp;
}

struct demux_packet *new_demux_packet_from(void *data, size_t len)
{
    struct demux_packet *dp = new_demux_packet(len);
    memcpy(dp->buffer, data, len);
    return dp;
}

void resize_demux_packet(struct demux_packet *dp, size_t len)
{
    if (len > 1000000000) {
        mp_msg(MSGT_DEMUXER, MSGL_FATAL, "Attempt to realloc demux packet "
               "over 1 GB!\n");
        abort();
    }
    assert(dp->allocation);
    dp->buffer = realloc(dp->buffer, len + MP_INPUT_BUFFER_PADDING_SIZE);
    if (!dp->buffer) {
        mp_msg(MSGT_DEMUXER, MSGL_FATAL, "Memory allocation failure!\n");
        abort();
    }
    memset(dp->buffer + len, 0, MP_INPUT_BUFFER_PADDING_SIZE);
    dp->len = len;
    dp->allocation = dp->buffer;
}

void free_demux_packet(struct demux_packet *dp)
{
    talloc_free(dp);
}

static void destroy_avpacket(void *pkt)
{
    av_free_packet(pkt);
}

struct demux_packet *demux_copy_packet(struct demux_packet *dp)
{
    struct demux_packet *new = NULL;
    // No av_copy_packet() in Libav
#if LIBAVCODEC_VERSION_MICRO >= 100
    if (dp->avpacket) {
        assert(dp->buffer == dp->avpacket->data);
        assert(dp->len == dp->avpacket->size);
        AVPacket *newavp = talloc_zero(NULL, AVPacket);
        talloc_set_destructor(newavp, destroy_avpacket);
        av_init_packet(newavp);
        if (av_copy_packet(newavp, dp->avpacket) < 0)
            abort();
        new = new_demux_packet_fromdata(newavp->data, newavp->size);
        new->avpacket = newavp;
    }
#endif
    if (!new) {
        new = new_demux_packet(dp->len);
        memcpy(new->buffer, dp->buffer, new->len);
    }
    new->pts = dp->pts;
    new->duration = dp->duration;
    new->stream_pts = dp->stream_pts;
    return new;
}

struct sh_stream *new_sh_stream(demuxer_t *demuxer, enum stream_type type)
{
    if (demuxer->num_streams > MAX_SH_STREAMS) {
        mp_msg(MSGT_DEMUXER, MSGL_WARN, "Too many streams.");
        return NULL;
    }

    int demuxer_id = 0;
    for (int n = 0; n < demuxer->num_streams; n++) {
        if (demuxer->streams[n]->type == type)
            demuxer_id++;
    }

    struct sh_stream *sh = talloc_ptrtype(demuxer, sh);
    *sh = (struct sh_stream) {
        .type = type,
        .demuxer = demuxer,
        .index = demuxer->num_streams,
        .demuxer_id = demuxer_id, // may be overwritten by demuxer
        .pq = packet_queue_create(sh),
    };
    MP_TARRAY_APPEND(demuxer, demuxer->streams, demuxer->num_streams, sh);
    switch (sh->type) {
        case STREAM_VIDEO: {
            struct sh_video *sht = talloc_zero(demuxer, struct sh_video);
            sh->video = sht;
            break;
        }
        case STREAM_AUDIO: {
            struct sh_audio *sht = talloc_zero(demuxer, struct sh_audio);
            sh->audio = sht;
            break;
        }
        case STREAM_SUB: {
            struct sh_sub *sht = talloc_zero(demuxer, struct sh_sub);
            sh->sub = sht;
            break;
        }
        default: assert(false);
    }

    sh->selected = demuxer->stream_autoselect;

    return sh;
}

void free_demuxer(demuxer_t *demuxer)
{
    if (!demuxer)
        return;
    if (demuxer->desc->close)
        demuxer->desc->close(demuxer);
    talloc_free(demuxer);
}

static const char *stream_type_name(enum stream_type type)
{
    switch (type) {
    case STREAM_VIDEO:  return "video";
    case STREAM_AUDIO:  return "audio";
    case STREAM_SUB:    return "sub";
    default:            return "unknown";
    }
}

static void get_queue_sizes(struct demuxer *demux, size_t *bytes, int *count)
{
    memset(bytes, 0, sizeof(bytes[0]) * STREAM_TYPE_COUNT);
    memset(count, 0, sizeof(count[0]) * STREAM_TYPE_COUNT);
    for (int n = 0; n < demux->num_streams; n++) {
        struct sh_stream *sh = demux->streams[n];
        packet_queue_add_size(sh->pq, &bytes[sh->type], &count[sh->type]);
    }
}

// Returns the same value as demuxer->fill_buffer: 1 ok, 0 EOF/not selected.
int demuxer_add_packet(demuxer_t *demuxer, struct sh_stream *stream,
                       demux_packet_t *dp)
{
    if (!dp || !stream || !stream->selected) {
        printf("kill\n");
        talloc_free(dp);
        return 0;
    }

    dp->stream = stream->index;
    dp->next = NULL;

    if (dp->pos >= 0)
        demuxer->filepos = dp->pos;

    struct demux_packet info = *dp; // can't access dp after add

    packet_queue_add(stream->pq, dp);

    size_t qbytes[STREAM_TYPE_COUNT];
    int qcount[STREAM_TYPE_COUNT];
    get_queue_sizes(demuxer, qbytes, qcount);

    mp_dbg(MSGT_DEMUXER, MSGL_DBG2,
           "DEMUX: Append packet to %s, len=%d  pts=%5.3f  pos=%"PRIu64" "
           "[packs: A=%d V=%d S=%d]\n", stream_type_name(stream->type),
           info.len, info.pts, info.pos,
           qcount[STREAM_AUDIO], qcount[STREAM_VIDEO], qcount[STREAM_SUB]);
    return 1;
}

static bool demux_check_queue_full(demuxer_t *demux)
{
    for (int n = 0; n < demux->num_streams; n++) {
        struct sh_stream *sh = demux->streams[n];
        size_t bytes = 0;
        int count = 0;
        packet_queue_add_size(sh->pq, &bytes, &count);
        if (count > MAX_PACKS || bytes > MAX_PACK_BYTES)
            goto overflow;
    }
    return false;

overflow:

    if (!demux->warned_queue_overflow) {
        size_t qbytes[STREAM_TYPE_COUNT];
        int qcount[STREAM_TYPE_COUNT];
        get_queue_sizes(demux, qbytes, qcount);

        mp_tmsg(MSGT_DEMUXER, MSGL_ERR, "\nToo many packets in the demuxer "
                "packet queue (video: %d packets in %d bytes, audio: %d "
                "packets in %d bytes, sub: %d packets in %d bytes).\n",
                qcount[STREAM_VIDEO], qbytes[STREAM_VIDEO],
                qcount[STREAM_AUDIO], qbytes[STREAM_AUDIO],
                qcount[STREAM_SUB], qbytes[STREAM_SUB]);
        mp_tmsg(MSGT_DEMUXER, MSGL_HINT, "Maybe you are playing a non-"
                "interleaved stream/file or the codec failed?\n");
    }
    demux->warned_queue_overflow = true;
    return true;
}

// return value:
//     0 = EOF or no stream found or invalid type
//     1 = successfully read a packet

static int demux_fill_buffer(demuxer_t *demux)
{
    return demux->desc->fill_buffer ? demux->desc->fill_buffer(demux) : 0;
}

static void ds_get_packets(struct sh_stream *sh)
{
    demuxer_t *demux = sh->demuxer;
    mp_dbg(MSGT_DEMUXER, MSGL_DBG3, "ds_get_packets (%s) called\n",
           stream_type_name(sh->type));
    while (1) {
        if (!packet_queue_is_empty(sh->pq))
            return;

        if (demux_check_queue_full(demux))
            break;

        if (!demux_fill_buffer(demux))
            break; // EOF
    }
    mp_msg(MSGT_DEMUXER, MSGL_V, "ds_get_packets: EOF reached (stream: %s)\n",
           stream_type_name(sh->type));
    packet_queue_set_eof(sh->pq, true);
}

// Read a packet from the given stream. The returned packet belongs to the
// caller, who has to free it with talloc_free(). Might block. Returns NULL
// on EOF.
struct demux_packet *demux_read_packet(struct sh_stream *sh)
{
    if (!sh)
        return NULL;
    ds_get_packets(sh);
    struct demux_packet *dp = packet_queue_get(sh->pq);
    if (dp && dp->stream_pts != MP_NOPTS_VALUE)
        sh->demuxer->stream_pts = dp->stream_pts;
    return dp;
}

// Return the pts of the next packet that demux_read_packet() would return.
// Might block. Sometimes used to force a packet read, without removing any
// packets from the queue.
double demux_get_next_pts(struct sh_stream *sh)
{
    return sh ? packet_queue_get_pts(sh->pq) : MP_NOPTS_VALUE;
}

// Return whether a packet is queued. Never blocks, never forces any reads.
bool demux_has_packet(struct sh_stream *sh)
{
    return sh && !packet_queue_is_empty(sh->pq);
}

// Same as demux_has_packet, but to be called internally by demuxers, as
// opposed to the user of the demuxer.
bool demuxer_stream_has_packets_queued(struct demuxer *d, struct sh_stream *stream)
{
    return demux_has_packet(stream);
}

// Return whether EOF was returned with an earlier packet read.
bool demux_stream_eof(struct sh_stream *sh)
{
    return !sh || packet_queue_is_eof(sh->pq);
}

// ====================================================================

void demuxer_help(void)
{
    int i;

    mp_msg(MSGT_DEMUXER, MSGL_INFO, "Available demuxers:\n");
    mp_msg(MSGT_DEMUXER, MSGL_INFO, " demuxer:   info:\n");
    mp_msg(MSGT_IDENTIFY, MSGL_INFO, "ID_DEMUXERS\n");
    for (i = 0; demuxer_list[i]; i++) {
        mp_msg(MSGT_DEMUXER, MSGL_INFO, "%10s  %s\n",
               demuxer_list[i]->name, demuxer_list[i]->desc);
    }
}

static const char *d_level(enum demux_check level)
{
    switch (level) {
    case DEMUX_CHECK_FORCE:  return "force";
    case DEMUX_CHECK_UNSAFE: return "unsafe";
    case DEMUX_CHECK_REQUEST:return "request";
    case DEMUX_CHECK_NORMAL: return "normal";
    }
    abort();
}

static struct demuxer *open_given_type(struct MPOpts *opts,
                                       const struct demuxer_desc *desc,
                                       struct stream *stream,
                                       struct demuxer_params *params,
                                       enum demux_check check)
{
    struct demuxer *demuxer = talloc_ptrtype(NULL, demuxer);
    *demuxer = (struct demuxer) {
        .desc = desc,
        .type = desc->type,
        .stream = stream,
        .stream_pts = MP_NOPTS_VALUE,
        .seekable = (stream->flags & MP_STREAM_SEEK) == MP_STREAM_SEEK &&
                    stream->end_pos > 0,
        .accurate_seek = true,
        .filepos = -1,
        .opts = opts,
        .filename = talloc_strdup(demuxer, stream->url),
        .metadata = talloc_zero(demuxer, struct mp_tags),
    };
    demuxer->params = params; // temporary during open()
    stream_seek(stream, stream->start_pos);

    mp_msg(MSGT_DEMUXER, MSGL_V, "Trying demuxer: %s (force-level: %s)\n",
           desc->name, d_level(check));

    int ret = demuxer->desc->open(demuxer, check);
    if (ret >= 0) {
        demuxer->params = NULL;
        if (demuxer->filetype)
            mp_tmsg(MSGT_DEMUXER, MSGL_INFO, "Detected file format: %s (%s)\n",
                    demuxer->filetype, desc->desc);
        else
            mp_tmsg(MSGT_DEMUXER, MSGL_INFO, "Detected file format: %s\n",
                    desc->desc);
        if (stream_manages_timeline(demuxer->stream)) {
            // Incorrect, but fixes some behavior with DVD/BD
            demuxer->ts_resets_possible = false;
            // Doesn't work, because stream_pts is a "guess".
            demuxer->accurate_seek = false;
        }
        add_stream_chapters(demuxer);
        demuxer_sort_chapters(demuxer);
        demux_info_update(demuxer);
        // Pretend we can seek if we can't seek, but there's a cache.
        if (!demuxer->seekable && stream->uncached_stream) {
            mp_msg(MSGT_DEMUXER, MSGL_WARN,
                   "File is not seekable, but there's a cache: enabling seeking.\n");
            demuxer->seekable = true;
        }
        return demuxer;
    }

    free_demuxer(demuxer);
    return NULL;
}

static const int d_normal[]  = {DEMUX_CHECK_NORMAL, DEMUX_CHECK_UNSAFE, -1};
static const int d_request[] = {DEMUX_CHECK_REQUEST, -1};
static const int d_force[]   = {DEMUX_CHECK_FORCE, -1};

struct demuxer *demux_open(struct stream *stream, char *force_format,
                           struct demuxer_params *params, struct MPOpts *opts)
{
    const int *check_levels = d_normal;
    const struct demuxer_desc *check_desc = NULL;

    if (!force_format)
        force_format = stream->demuxer;

    if (force_format && force_format[0]) {
        check_levels = d_request;
        if (force_format[0] == '+') {
            force_format += 1;
            check_levels = d_force;
        }
        for (int n = 0; demuxer_list[n]; n++) {
            if (strcmp(demuxer_list[n]->name, force_format) == 0)
                check_desc = demuxer_list[n];
        }
        if (!check_desc) {
            mp_msg(MSGT_DEMUXER, MSGL_ERR, "Demuxer %s does not exist.\n",
                   force_format);
            return NULL;
        }
    }

    // Peek this much data to avoid that stream_read() run by some demuxers
    // or stream filters will flush previous peeked data.
    stream_peek(stream, STREAM_BUFFER_SIZE);

    // Test demuxers from first to last, one pass for each check_levels[] entry
    for (int pass = 0; check_levels[pass] != -1; pass++) {
        enum demux_check level = check_levels[pass];
        for (int n = 0; demuxer_list[n]; n++) {
            const struct demuxer_desc *desc = demuxer_list[n];
            if (!check_desc || desc == check_desc) {
                struct demuxer *demuxer = open_given_type(opts, desc, stream,
                                                          params, level);
                if (demuxer)
                    return demuxer;
            }
        }
    }

    return NULL;
}

void demux_flush(demuxer_t *demuxer)
{
    for (int n = 0; n < demuxer->num_streams; n++)
        packet_queue_flush(demuxer->streams[n]->pq);
    demuxer->warned_queue_overflow = false;
}

int demux_seek(demuxer_t *demuxer, float rel_seek_secs, int flags)
{
    if (!demuxer->seekable) {
        mp_tmsg(MSGT_DEMUXER, MSGL_WARN, "Cannot seek in this file.\n");
        return 0;
    }

    if (rel_seek_secs == MP_NOPTS_VALUE && (flags & SEEK_ABSOLUTE))
        return 0;

    // clear demux buffers:
    demux_flush(demuxer);

    /* Note: this is for DVD and BD playback. The stream layer has to do these
     * seeks, and the demuxer has to react to DEMUXER_CTRL_RESYNC in order to
     * deal with the suddenly changing stream position.
     */
    struct stream *stream = demuxer->stream;
    if (stream_manages_timeline(stream)) {
        double pts;

        if (flags & SEEK_ABSOLUTE)
            pts = 0.0f;
        else {
            if (demuxer->stream_pts == MP_NOPTS_VALUE)
                goto dmx_seek;
            pts = demuxer->stream_pts;
        }

        if (flags & SEEK_FACTOR) {
            double tmp = 0;
            if (stream_control(demuxer->stream, STREAM_CTRL_GET_TIME_LENGTH,
                               &tmp) == STREAM_UNSUPPORTED)
                goto dmx_seek;
            pts += tmp * rel_seek_secs;
        } else
            pts += rel_seek_secs;

        if (stream_control(demuxer->stream, STREAM_CTRL_SEEK_TO_TIME, &pts)
            != STREAM_UNSUPPORTED) {
            demux_control(demuxer, DEMUXER_CTRL_RESYNC, NULL);
            return 1;
        }
    }

  dmx_seek:
    if (demuxer->desc->seek)
        demuxer->desc->seek(demuxer, rel_seek_secs, flags);

    return 1;
}

void mp_tags_set_str(struct mp_tags *tags, const char *key, const char *value)
{
    mp_tags_set_bstr(tags, bstr0(key), bstr0(value));
}

void mp_tags_set_bstr(struct mp_tags *tags, bstr key, bstr value)
{
    for (int n = 0; n < tags->num_keys; n++) {
        if (bstrcasecmp0(key, tags->keys[n]) == 0) {
            talloc_free(tags->values[n]);
            tags->values[n] = talloc_strndup(tags, value.start, value.len);
            return;
        }
    }

    MP_RESIZE_ARRAY(tags, tags->keys,   tags->num_keys + 1);
    MP_RESIZE_ARRAY(tags, tags->values, tags->num_keys + 1);
    tags->keys[tags->num_keys]   = talloc_strndup(tags, key.start,   key.len);
    tags->values[tags->num_keys] = talloc_strndup(tags, value.start, value.len);
    tags->num_keys++;
}

char *mp_tags_get_str(struct mp_tags *tags, const char *key)
{
    return mp_tags_get_bstr(tags, bstr0(key));
}

char *mp_tags_get_bstr(struct mp_tags *tags, bstr key)
{
    for (int n = 0; n < tags->num_keys; n++) {
        if (bstrcasecmp0(key, tags->keys[n]) == 0)
            return tags->values[n];
    }
    return NULL;
}

int demux_info_add(demuxer_t *demuxer, const char *opt, const char *param)
{
    return demux_info_add_bstr(demuxer, bstr0(opt), bstr0(param));
}

int demux_info_add_bstr(demuxer_t *demuxer, struct bstr opt, struct bstr param)
{
    char *oldval = mp_tags_get_bstr(demuxer->metadata, opt);
    if (oldval) {
        if (bstrcmp0(param, oldval) == 0)
            return 0;
        mp_tmsg(MSGT_DEMUX, MSGL_INFO, "Demuxer info %.*s changed to %.*s\n",
                BSTR_P(opt), BSTR_P(param));
    }

    mp_tags_set_bstr(demuxer->metadata, opt, param);
    return 1;
}

int demux_info_print(demuxer_t *demuxer)
{
    struct mp_tags *info = demuxer->metadata;
    int n;

    if (!info || !info->num_keys)
        return 0;

    mp_tmsg(MSGT_DEMUX, MSGL_INFO, "Clip info:\n");
    for (n = 0; n < info->num_keys; n++) {
        mp_msg(MSGT_DEMUX, MSGL_INFO, " %s: %s\n", info->keys[n],
               info->values[n]);
        mp_msg(MSGT_IDENTIFY, MSGL_INFO, "ID_CLIP_INFO_NAME%d=%s\n", n,
               info->keys[n]);
        mp_msg(MSGT_IDENTIFY, MSGL_INFO, "ID_CLIP_INFO_VALUE%d=%s\n", n,
               info->values[n]);
    }
    mp_msg(MSGT_IDENTIFY, MSGL_INFO, "ID_CLIP_INFO_N=%d\n", n);

    return 0;
}

char *demux_info_get(demuxer_t *demuxer, const char *opt)
{
    return mp_tags_get_str(demuxer->metadata, opt);
}

void demux_info_update(struct demuxer *demuxer)
{
    demux_control(demuxer, DEMUXER_CTRL_UPDATE_INFO, NULL);
    // Take care of stream metadata as well
    char **meta;
    if (stream_control(demuxer->stream, STREAM_CTRL_GET_METADATA, &meta) > 0) {
        for (int n = 0; meta[n + 0]; n += 2)
            demux_info_add(demuxer, meta[n + 0], meta[n + 1]);
        talloc_free(meta);
    }
}

int demux_control(demuxer_t *demuxer, int cmd, void *arg)
{

    if (demuxer->desc->control)
        return demuxer->desc->control(demuxer, cmd, arg);

    return DEMUXER_CTRL_NOTIMPL;
}

struct sh_stream *demuxer_stream_by_demuxer_id(struct demuxer *d,
                                               enum stream_type t, int id)
{
    for (int n = 0; n < d->num_streams; n++) {
        struct sh_stream *s = d->streams[n];
        if (s->type == t && s->demuxer_id == id)
            return d->streams[n];
    }
    return NULL;
}

void demuxer_switch_track(struct demuxer *demuxer, enum stream_type type,
                          struct sh_stream *stream)
{
    assert(!stream || stream->type == type);

    for (int n = 0; n < demuxer->num_streams; n++) {
        struct sh_stream *cur = demuxer->streams[n];
        if (cur->type == type)
            demuxer_select_track(demuxer, cur, cur == stream);
    }
}

void demuxer_select_track(struct demuxer *demuxer, struct sh_stream *stream,
                          bool selected)
{
    // don't flush buffers if stream is already selected / unselected
    if (stream->selected != selected) {
        stream->selected = selected;
        packet_queue_flush(stream->pq);
        demux_control(demuxer, DEMUXER_CTRL_SWITCHED_TRACKS, NULL);
    }
}

void demuxer_enable_autoselect(struct demuxer *demuxer)
{
    demuxer->stream_autoselect = true;
}

bool demuxer_stream_is_selected(struct demuxer *d, struct sh_stream *stream)
{
    return stream && stream->selected;
}

int demuxer_add_attachment(demuxer_t *demuxer, struct bstr name,
                           struct bstr type, struct bstr data)
{
    if (!(demuxer->num_attachments % 32))
        demuxer->attachments = talloc_realloc(demuxer, demuxer->attachments,
                                              struct demux_attachment,
                                              demuxer->num_attachments + 32);

    struct demux_attachment *att =
        demuxer->attachments + demuxer->num_attachments;
    att->name = talloc_strndup(demuxer->attachments, name.start, name.len);
    att->type = talloc_strndup(demuxer->attachments, type.start, type.len);
    att->data = talloc_size(demuxer->attachments, data.len);
    memcpy(att->data, data.start, data.len);
    att->data_size = data.len;

    return demuxer->num_attachments++;
}

static int chapter_compare(const void *p1, const void *p2)
{
    struct demux_chapter *c1 = (void *)p1;
    struct demux_chapter *c2 = (void *)p2;

    if (c1->start > c2->start)
        return 1;
    else if (c1->start < c2->start)
        return -1;
    return c1->original_index > c2->original_index ? 1 :-1; // never equal
}

void demuxer_sort_chapters(demuxer_t *demuxer)
{
    qsort(demuxer->chapters, demuxer->num_chapters,
          sizeof(struct demux_chapter), chapter_compare);
}

int demuxer_add_chapter(demuxer_t *demuxer, struct bstr name,
                        uint64_t start, uint64_t end, uint64_t demuxer_id)
{
    struct demux_chapter new = {
        .original_index = demuxer->num_chapters,
        .start = start,
        .end = end,
        .name = name.len ? bstrdup0(demuxer, name) : NULL,
        .metadata = talloc_zero(demuxer, struct mp_tags),
        .demuxer_id = demuxer_id,
    };
    mp_tags_set_bstr(new.metadata, bstr0("TITLE"), name);
    MP_TARRAY_APPEND(demuxer, demuxer->chapters, demuxer->num_chapters, new);
    return 0;
}

void demuxer_add_chapter_info(struct demuxer *demuxer, uint64_t demuxer_id,
                              bstr key, bstr value)
{
    for (int n = 0; n < demuxer->num_chapters; n++) {
        struct demux_chapter *ch = &demuxer->chapters[n];
        if (ch->demuxer_id == demuxer_id) {
            mp_tags_set_bstr(ch->metadata, key, value);
            return;
        }
    }
}

static void add_stream_chapters(struct demuxer *demuxer)
{
    if (demuxer->num_chapters)
        return;
    int num_chapters = demuxer_chapter_count(demuxer);
    for (int n = 0; n < num_chapters; n++) {
        double p = n;
        if (stream_control(demuxer->stream, STREAM_CTRL_GET_CHAPTER_TIME, &p)
                != STREAM_OK)
            return;
        demuxer_add_chapter(demuxer, bstr0(""), p * 1e9, 0, 0);
    }
}

/**
 * \brief demuxer_seek_chapter() seeks to a chapter in two possible ways:
 *        either using the demuxer->chapters structure set by the demuxer
 *        or asking help to the stream layer (e.g. dvd)
 * \param chapter - chapter number wished - 0-based
 * \param seek_pts set by the function to the pts to seek to (if demuxer->chapters is set)
 * \return -1 on error, current chapter if successful
 */

int demuxer_seek_chapter(demuxer_t *demuxer, int chapter, double *seek_pts)
{
    int ris = STREAM_UNSUPPORTED;

    if (demuxer->num_chapters == 0)
        ris = stream_control(demuxer->stream, STREAM_CTRL_SEEK_TO_CHAPTER,
                             &chapter);

    if (ris != STREAM_UNSUPPORTED) {
        demux_flush(demuxer);
        demux_control(demuxer, DEMUXER_CTRL_RESYNC, NULL);

        // exit status may be ok, but main() doesn't have to seek itself
        // (because e.g. dvds depend on sectors, not on pts)
        *seek_pts = -1.0;

        return chapter;
    } else {
        if (chapter >= demuxer->num_chapters)
            return -1;
        if (chapter < 0)
            chapter = 0;

        *seek_pts = demuxer->chapters[chapter].start / 1e9;

        return chapter;
    }
}

int demuxer_get_current_chapter(demuxer_t *demuxer, double time_now)
{
    int chapter = -2;
    if (!demuxer->num_chapters || !demuxer->chapters) {
        if (stream_control(demuxer->stream, STREAM_CTRL_GET_CURRENT_CHAPTER,
                           &chapter) == STREAM_UNSUPPORTED)
            chapter = -2;
    } else {
        uint64_t now = time_now * 1e9 + 0.5;
        for (chapter = demuxer->num_chapters - 1; chapter >= 0; --chapter) {
            if (demuxer->chapters[chapter].start <= now)
                break;
        }
    }
    return chapter;
}

char *demuxer_chapter_name(demuxer_t *demuxer, int chapter)
{
    if (demuxer->num_chapters && demuxer->chapters) {
        if (chapter >= 0 && chapter < demuxer->num_chapters
            && demuxer->chapters[chapter].name)
            return talloc_strdup(NULL, demuxer->chapters[chapter].name);
    }
    return NULL;
}

double demuxer_chapter_time(demuxer_t *demuxer, int chapter)
{
    if (demuxer->num_chapters && demuxer->chapters && chapter >= 0
        && chapter < demuxer->num_chapters) {
        return demuxer->chapters[chapter].start / 1e9;
    }
    return -1.0;
}

int demuxer_chapter_count(demuxer_t *demuxer)
{
    if (!demuxer->num_chapters || !demuxer->chapters) {
        int num_chapters = 0;
        if (stream_control(demuxer->stream, STREAM_CTRL_GET_NUM_CHAPTERS,
                           &num_chapters) == STREAM_UNSUPPORTED)
            num_chapters = 0;
        return num_chapters;
    } else
        return demuxer->num_chapters;
}

double demuxer_get_time_length(struct demuxer *demuxer)
{
    double len;
    if (stream_control(demuxer->stream, STREAM_CTRL_GET_TIME_LENGTH, &len) > 0)
        return len;
    // <= 0 means DEMUXER_CTRL_NOTIMPL or DEMUXER_CTRL_DONTKNOW
    if (demux_control(demuxer, DEMUXER_CTRL_GET_TIME_LENGTH, &len) > 0)
        return len;
    return -1;
}

double demuxer_get_start_time(struct demuxer *demuxer)
{
    double time;
    if (stream_control(demuxer->stream, STREAM_CTRL_GET_START_TIME, &time) > 0)
        return time;
    if (demux_control(demuxer, DEMUXER_CTRL_GET_START_TIME, &time) > 0)
        return time;
    return 0;
}

int demuxer_angles_count(demuxer_t *demuxer)
{
    int ris, angles = -1;

    ris = stream_control(demuxer->stream, STREAM_CTRL_GET_NUM_ANGLES, &angles);
    if (ris == STREAM_UNSUPPORTED)
        return -1;
    return angles;
}

int demuxer_get_current_angle(demuxer_t *demuxer)
{
    int ris, curr_angle = -1;
    ris = stream_control(demuxer->stream, STREAM_CTRL_GET_ANGLE, &curr_angle);
    if (ris == STREAM_UNSUPPORTED)
        return -1;
    return curr_angle;
}


int demuxer_set_angle(demuxer_t *demuxer, int angle)
{
    int ris, angles = -1;

    angles = demuxer_angles_count(demuxer);
    if ((angles < 1) || (angle > angles))
        return -1;

    demux_flush(demuxer);

    ris = stream_control(demuxer->stream, STREAM_CTRL_SET_ANGLE, &angle);
    if (ris == STREAM_UNSUPPORTED)
        return -1;

    demux_control(demuxer, DEMUXER_CTRL_RESYNC, NULL);

    return angle;
}

static int packet_sort_compare(const void *p1, const void *p2)
{
    struct demux_packet *c1 = *(struct demux_packet **)p1;
    struct demux_packet *c2 = *(struct demux_packet **)p2;

    if (c1->pts > c2->pts)
        return 1;
    else if (c1->pts < c2->pts)
        return -1;
    return 0;
}

void demux_packet_list_sort(struct demux_packet **pkts, int num_pkts)
{
    qsort(pkts, num_pkts, sizeof(struct demux_packet *), packet_sort_compare);
}

void demux_packet_list_seek(struct demux_packet **pkts, int num_pkts,
                            int *current, float rel_seek_secs, int flags)
{
    double ref_time = 0;
    if (*current >= 0 && *current < num_pkts) {
        ref_time = pkts[*current]->pts;
    } else if (*current == num_pkts && num_pkts > 0) {
        ref_time = pkts[num_pkts - 1]->pts + pkts[num_pkts - 1]->duration;
    }

    if (flags & SEEK_ABSOLUTE)
        ref_time = 0;

    if (flags & SEEK_FACTOR) {
        ref_time += demux_packet_list_duration(pkts, num_pkts) * rel_seek_secs;
    } else {
        ref_time += rel_seek_secs;
    }

    // Could do binary search, but it's probably not worth the complexity.
    int last_index = 0;
    for (int n = 0; n < num_pkts; n++) {
        if (pkts[n]->pts > ref_time)
            break;
        last_index = n;
    }
    *current = last_index;
}

double demux_packet_list_duration(struct demux_packet **pkts, int num_pkts)
{
    if (num_pkts > 0)
        return pkts[num_pkts - 1]->pts + pkts[num_pkts - 1]->duration;
    return 0;
}

struct demux_packet *demux_packet_list_fill(struct demux_packet **pkts,
                                            int num_pkts, int *current)
{
    if (*current < 0)
        *current = 0;
    if (*current >= num_pkts)
        return NULL;
    struct demux_packet *new = talloc(NULL, struct demux_packet);
    *new = *pkts[*current];
    *current += 1;
    return new;
}
