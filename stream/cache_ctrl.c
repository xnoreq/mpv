
#include "mpvcore/mp_common.h"

#include "cache_ctrl.h"
#include "stream.h"

// Update the cache using stream s.
// The caller needs to take care of synchronization.
// Note: "cache" needs to be allocated with talloc_zero(). This function
//       possibly allocates memory, for which "cache" is set as parent.
void stream_cache_ctrl_update(struct stream_cache_ctrls *cache, struct stream *s)
{
    unsigned int ui;
    double d;
    char **m;
    int i;
    int64_t i64;
    cache->stream_cache_size = 0;
    if (stream_control(s, STREAM_CTRL_GET_CACHE_SIZE, &i64) == STREAM_OK)
        cache->stream_cache_size = i64;
    cache->stream_cache_fill = 0;
    if (stream_control(s, STREAM_CTRL_GET_CACHE_FILL, &i64) == STREAM_OK)
        cache->stream_cache_fill = i64;
    cache->stream_cache_idle = 0;
    if (stream_control(s, STREAM_CTRL_GET_CACHE_IDLE, &i) == STREAM_OK)
        cache->stream_cache_idle = i;
    cache->stream_time_length = 0;
    if (stream_control(s, STREAM_CTRL_GET_TIME_LENGTH, &d) == STREAM_OK)
        cache->stream_time_length = d;
    cache->stream_start_time = MP_NOPTS_VALUE;
    if (stream_control(s, STREAM_CTRL_GET_START_TIME, &d) == STREAM_OK)
        cache->stream_start_time = d;
    cache->stream_manages_timeline = false;
    if (stream_control(s, STREAM_CTRL_MANAGES_TIMELINE, NULL) == STREAM_OK)
        cache->stream_manages_timeline = true;
    cache->stream_num_chapters = 0;
    if (stream_control(s, STREAM_CTRL_GET_NUM_CHAPTERS, &ui) == STREAM_OK)
        cache->stream_num_chapters = ui;
    if (stream_control(s, STREAM_CTRL_GET_METADATA, &m) == STREAM_OK) {
        talloc_free(cache->stream_metadata);
        cache->stream_metadata = talloc_steal(s, m);
    }
    stream_update_size(s);
    cache->stream_size = s->end_pos;
}

// Try to handle STREAM_CTRLs with the cache.
// Returns STREAM_ERROR if a STREAM_CTRL is not covered by the cache.
int stream_cache_ctrl_get(struct stream_cache_ctrls *cache, int cmd, void *arg)
{
    switch (cmd) {
    case STREAM_CTRL_GET_CACHE_SIZE:
        *(int64_t *)arg = cache->stream_cache_size;
        return cache->stream_cache_size ? STREAM_OK : STREAM_UNSUPPORTED;
    case STREAM_CTRL_GET_CACHE_FILL:
        *(int64_t *)arg = cache->stream_cache_fill;
        return STREAM_OK; // not perfectly correct
    case STREAM_CTRL_GET_CACHE_IDLE:
        *(int *)arg = cache->stream_cache_idle;
        return STREAM_OK; // not perfectly correct
    case STREAM_CTRL_GET_TIME_LENGTH:
        *(double *)arg = cache->stream_time_length;
        return cache->stream_time_length ? STREAM_OK : STREAM_UNSUPPORTED;
    case STREAM_CTRL_GET_START_TIME:
        *(double *)arg = cache->stream_start_time;
        return cache->stream_start_time !=
               MP_NOPTS_VALUE ? STREAM_OK : STREAM_UNSUPPORTED;
    case STREAM_CTRL_GET_SIZE:
        *(int64_t *)arg = cache->stream_size;
        return STREAM_OK;
    case STREAM_CTRL_MANAGES_TIMELINE:
        return cache->stream_manages_timeline ? STREAM_OK : STREAM_UNSUPPORTED;
    case STREAM_CTRL_GET_NUM_CHAPTERS:
        *(unsigned int *)arg = cache->stream_num_chapters;
        return STREAM_OK;
    case STREAM_CTRL_GET_METADATA: {
        if (cache->stream_metadata && cache->stream_metadata[0]) {
            char **m = talloc_new(NULL);
            int num_m = 0;
            for (int n = 0; cache->stream_metadata[n]; n++) {
                char *t = talloc_strdup(m, cache->stream_metadata[n]);
                MP_TARRAY_APPEND(NULL, m, num_m, t);
            }
            MP_TARRAY_APPEND(NULL, m, num_m, NULL);
            MP_TARRAY_APPEND(NULL, m, num_m, NULL);
            *(char ***)arg = m;
            return STREAM_OK;
        }
        return STREAM_UNSUPPORTED;
    }
    }
    return STREAM_ERROR;
}

// Return whether the given STREAM_CTRL executes a seek, or a similar operation
// that requires a cache flush. (Both stream_cache_ctrls and cached data have
// to be flushed.)
bool stream_cache_ctrl_needs_flush(int stream_ctrl)
{
    switch (stream_ctrl) {
    case STREAM_CTRL_SEEK_TO_TIME:
    case STREAM_CTRL_SEEK_TO_CHAPTER:
    case STREAM_CTRL_SET_ANGLE:
        return true;
    }
    return false;
}
