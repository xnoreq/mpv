#ifndef MP_CACHE_CTRL_H_
#define MP_CACHE_CTRL_H_

#include <stdbool.h>
#include <inttypes.h>

struct stream_cache_ctrls {
    double stream_time_length;
    double stream_start_time;
    int64_t stream_size;
    bool stream_manages_timeline;
    unsigned int stream_num_chapters;
    int stream_cache_idle;
    int64_t stream_cache_fill;
    int64_t stream_cache_size;
    char **stream_metadata;
};

struct stream;

void stream_cache_ctrl_update(struct stream_cache_ctrls *cache, struct stream *s);
int stream_cache_ctrl_get(struct stream_cache_ctrls *cache, int cmd, void *arg);
bool stream_cache_ctrl_needs_flush(int stream_ctrl);

#endif
