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

#include <stddef.h>
#include <pthread.h>

#include "packet.h"

#include "mpvcore/mp_common.h"

struct packet_queue {
    pthread_mutex_t mutex;
    bool eof;           // end of demuxed stream? (if no more packets demuxed)
    int packs;          // number of packets in queue
    size_t bytes;       // total bytes of packets in queue
    struct demux_packet *head;
    struct demux_packet *tail;
};

static void queue_dtor(void *ptr)
{
    struct packet_queue *pq = ptr;
    packet_queue_flush(pq);
    pthread_mutex_destroy(&pq->mutex);
}

// Create a packet queue. All the API functions are completely thread-safe.
// Free the queue with talloc_free()
struct packet_queue *packet_queue_create(void *talloc_ctx)
{
    struct packet_queue *pq = talloc_zero(talloc_ctx, struct packet_queue);
    talloc_set_destructor(pq, queue_dtor);
    pthread_mutex_init(&pq->mutex, NULL);
    return pq;
}

// Append the given packet to the queue. Ownership of the packet is transferred
// to the queue as well, and the caller must not access the packet any further.
void packet_queue_add(struct packet_queue *pq, struct demux_packet *dp)
{
    pthread_mutex_lock(&pq->mutex);

    dp->next = NULL;

    pq->packs++;
    pq->bytes += dp->len;
    if (pq->tail) {
        // next packet in stream
        pq->tail->next = dp;
        pq->tail = dp;
    } else {
        // first packet in stream
        pq->head = pq->tail = dp;
    }
    /* ds_get_packets() can set pq->eof to 1 when another stream runs out of
     * buffer space. That makes sense because in that situation the calling
     * code should not count on being able to demux more packets from this
     * stream. (Can happen with e.g. badly interleaved files.)
     * In this case, we didn't necessarily reach EOF, and new packet can
     * appear. */
    pq->eof = false;

    pthread_mutex_unlock(&pq->mutex);
}

// Unqueue and return the oldest packet from the queue.
// The caller has to free the packet with talloc_free().
struct demux_packet *packet_queue_get(struct packet_queue *pq)
{
    struct demux_packet *dp = NULL;

    pthread_mutex_lock(&pq->mutex);
    if (pq->head) {
        dp = pq->head;
        pq->head = dp->next;
        dp->next = NULL;
        if (!pq->head)
            pq->tail = NULL;
        pq->bytes -= dp->len;
        pq->packs--;

    }
    pthread_mutex_unlock(&pq->mutex);

    return dp;
}

// Return true if there is at least one packet available.
bool packet_queue_is_empty(struct packet_queue *pq)
{
    pthread_mutex_lock(&pq->mutex);
    bool r = !pq->head;
    pthread_mutex_unlock(&pq->mutex);
    return r;
}

// Return EOF flag.
bool packet_queue_is_eof(struct packet_queue *pq)
{
    pthread_mutex_lock(&pq->mutex);
    bool r = pq->eof;
    pthread_mutex_unlock(&pq->mutex);
    return r;
}

// Set EOF flag. However, if there are still packets, force EOF to false.
void packet_queue_set_eof(struct packet_queue *pq, bool state)
{
    pthread_mutex_lock(&pq->mutex);
    pq->eof = state && !pq->head;
    pthread_mutex_unlock(&pq->mutex);
}

// Get the PTS of the packet that packet_queue_get() would return.
double packet_queue_get_pts(struct packet_queue *pq)
{
    pthread_mutex_lock(&pq->mutex);
    double r = pq->head ? pq->head->pts : MP_NOPTS_VALUE;
    pthread_mutex_unlock(&pq->mutex);
    return r;
}

void packet_queue_flush(struct packet_queue *pq)
{
    pthread_mutex_lock(&pq->mutex);

    struct demux_packet *dp = pq->head;
    while (dp) {
        struct demux_packet *dn = dp->next;
        talloc_free(dp);
        dp = dn;
    }
    pq->head = pq->tail = NULL;
    pq->packs = pq->bytes = 0;
    pq->eof = false;

    pthread_mutex_unlock(&pq->mutex);
}

// Add the queue size to the given parameters.
void packet_queue_add_size(struct packet_queue *pq, size_t *size, int *count)
{
    pthread_mutex_lock(&pq->mutex);

    *size += pq->bytes;
    *count += pq->packs;

    pthread_mutex_unlock(&pq->mutex);
}
