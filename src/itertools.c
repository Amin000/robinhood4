/* This file is part of the RobinHood Library
 * Copyright (C) 2019 Commissariat a l'energie atomique et aux energies
 *                    alternatives
 *
 * SPDX-License-Identifer: LGPL-3.0-or-later
 *
 * author: Quentin Bouget <quentin.bouget@cea.fr>
 */

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "robinhood/itertools.h"
#include "robinhood/queue.h"

/*----------------------------------------------------------------------------*
 |                              rbh_iter_array()                              |
 *----------------------------------------------------------------------------*/

struct array_iterator {
    struct rbh_iterator iter;

    const char *array;
    size_t size;
    size_t count;

    size_t index;
};

static const void *
array_iter_next(void *iterator)
{
    struct array_iterator *array = iterator;

    if (array->index < array->count)
        return array->array + (array->size * array->index++);

    errno = ENODATA;
    return NULL;
}

static void
array_iter_destroy(void *iterator)
{
    free(iterator);
}

static const struct rbh_iterator_operations ARRAY_ITER_OPS = {
    .next = array_iter_next,
    .destroy = array_iter_destroy,
};

static const struct rbh_iterator ARRAY_ITER = {
    .ops = &ARRAY_ITER_OPS,
};

struct rbh_iterator *
rbh_iter_array(const void *array, size_t element_size, size_t element_count)
{
    struct array_iterator *iterator;

    iterator = malloc(sizeof(*iterator));
    if (iterator == NULL)
        return NULL;
    iterator->iter = ARRAY_ITER;

    iterator->array = array;
    iterator->size = element_size;
    iterator->count = element_count;

    iterator->index = 0;

    return &iterator->iter;
}

/*----------------------------------------------------------------------------*
 |                            rbh_mut_iter_array()                            |
 *----------------------------------------------------------------------------*/

struct rbh_mut_iterator *
rbh_mut_iter_array(void *array, size_t element_size, size_t element_count)
{
    return (struct rbh_mut_iterator *)rbh_iter_array(array, element_size,
                                                     element_count);
}

/*----------------------------------------------------------------------------*
 |                            rbh_iter_chunkify()                             |
 *----------------------------------------------------------------------------*/

struct chunk_iterator {
    struct rbh_iterator iterator;

    struct rbh_iterator *subiter;
    const void *first;
    size_t count;
    bool once;
};

static const void *
chunk_iter_next(void *iterator)
{
    struct chunk_iterator *chunk = iterator;
    const void *next;
    int save_errno;

    if (!chunk->once) {
        chunk->once = true;
        return chunk->first;
    }

    if (chunk->count == 0) {
        errno = ENODATA;
        return NULL;
    }

    save_errno = errno;
    errno = 0;
    next = rbh_iter_next(chunk->subiter);
    if (next != NULL || errno == 0)
        chunk->count--;

    errno = errno ? : save_errno;
    return next;
}

static void
chunk_iter_destroy(void *iterator)
{
    free(iterator);
}

static const struct rbh_iterator_operations CHUNK_ITER_OPS = {
    .next = chunk_iter_next,
    .destroy = chunk_iter_destroy,
};

static const struct rbh_iterator CHUNK_ITER = {
    .ops = &CHUNK_ITER_OPS,
};

struct chunkify_iterator {
    struct rbh_mut_iterator iterator;

    struct rbh_iterator *subiter;
    size_t chunk;
};

static void *
chunkify_iter_next(void *iterator)
{
    struct chunkify_iterator *chunkify = iterator;
    struct chunk_iterator *chunk;
    const void *first;
    int save_errno;

    save_errno = errno;
    errno = 0;
    first = rbh_iter_next(chunkify->subiter);
    if (first == NULL && errno != 0)
        return NULL;
    errno = save_errno;

    chunk = malloc(sizeof(*chunk));
    if (chunk == NULL)
        return NULL;

    chunk->iterator = CHUNK_ITER;

    chunk->subiter = chunkify->subiter;
    chunk->first = first;
    chunk->count = chunkify->chunk - 1;
    chunk->once = false;

    return chunk;
}

static void
chunkify_iter_destroy(void *iterator)
{
    struct chunkify_iterator *chunkify = iterator;

    rbh_iter_destroy(chunkify->subiter);
    free(chunkify);
}

static const struct rbh_mut_iterator_operations CHUNKIFY_ITER_OPS = {
    .next = chunkify_iter_next,
    .destroy = chunkify_iter_destroy,
};

static const struct rbh_mut_iterator CHUNKIFY_ITER = {
    .ops = &CHUNKIFY_ITER_OPS,
};

struct rbh_mut_iterator *
rbh_iter_chunkify(struct rbh_iterator *iterator, size_t chunk)
{
    struct chunkify_iterator *chunkify;

    if (chunk == 0) {
        errno = EINVAL;
        return NULL;
    }

    chunkify = malloc(sizeof(*chunkify));
    if (chunkify == NULL) {
        errno = ENOMEM;
        return NULL;
    }

    chunkify->iterator = CHUNKIFY_ITER;
    chunkify->subiter = iterator;
    chunkify->chunk = chunk;

    return &chunkify->iterator;
}

/*----------------------------------------------------------------------------*
 |                          rbh_mut_iter_chunkify()                           |
 *----------------------------------------------------------------------------*/

struct rbh_mut_iterator *
rbh_mut_iter_chunkify(struct rbh_mut_iterator *iterator, size_t chunk)
{
    return rbh_iter_chunkify((struct rbh_iterator *)iterator, chunk);
}

/*----------------------------------------------------------------------------*
 |                               rbh_iter_tee()                               |
 *----------------------------------------------------------------------------*/

struct tee_iterator {
    struct rbh_iterator iterator;

    struct rbh_iterator *subiter;
    struct tee_iterator *clone;
    struct rbh_queue *queue;
    const void *next;
};

static int
tee_iter_share(struct tee_iterator *tee, const void *element)
{
    return rbh_queue_push(tee->queue, &element, sizeof(element)) ? 0 : -1;
}

static const void *
_tee_iter_next(struct tee_iterator *tee)
{
    const void *element;

    if (tee->next) {
        element = tee->next;
        tee->next = NULL;
        return element;
    }

    return rbh_iter_next(tee->subiter);
}

static const void *
tee_iter_next(void *iterator)
{
    struct tee_iterator *tee = iterator;
    const void **queue_pointer;
    const void *element;
    size_t readable;
    int save_errno = errno;

    queue_pointer = rbh_queue_peek(tee->queue, &readable);
    if (readable) {
        assert(readable % sizeof(element) == 0);
        memcpy(&element, queue_pointer, sizeof(element));
        rbh_queue_pop(tee->queue, sizeof(element));
        return element;
    }

    /* If the last share failed, retry it now */
    if (tee->clone && tee->clone->next) {
        if (tee_iter_share(tee->clone, tee->clone->next))
            /* Cannot go any further without losing elements */
            return NULL;
        tee->clone->next = NULL;
    }

    errno = 0;
    element = _tee_iter_next(tee);
    if (element == NULL && errno != 0)
        return NULL;

    if (tee->clone != NULL) {
        /* Share this element this element for the other iterator */
        if (tee_iter_share(tee->clone, element))
            /* Sharing failed. Keep `element' somewhere the other side of the
             * tee can access. Will retry sharing later (if need be).
             */
            tee->clone->next = element;
    }

    errno = save_errno;
    return element;
}

static void
tee_iter_destroy(void *iterator)
{
    struct tee_iterator *tee = iterator;

    if (tee->clone != NULL)
        tee->clone->clone = NULL;
    else
        rbh_iter_destroy(tee->subiter);
    rbh_queue_destroy(tee->queue);
    free(tee);
}

static const struct rbh_iterator_operations TEE_ITER_OPS = {
    .next = tee_iter_next,
    .destroy = tee_iter_destroy,
};

static const struct rbh_iterator TEE_ITER = {
    .ops = &TEE_ITER_OPS,
};

static long page_size;

static void
get_page_size(void) __attribute__((constructor));

static void
get_page_size(void)
{
    page_size = sysconf(_SC_PAGESIZE);
}

static struct tee_iterator *
tee_iter_new(struct rbh_iterator *subiter)
{
    struct tee_iterator *tee;
    struct rbh_queue *queue;

    queue = rbh_queue_new(page_size);
    if (queue == NULL) {
        errno = ENOMEM;
        return NULL;
    }

    tee = malloc(sizeof(*tee));
    if (tee == NULL) {
        rbh_queue_destroy(queue);
        errno = ENOMEM;
        return NULL;
    }

    tee->iterator = TEE_ITER;
    tee->subiter = subiter;
    tee->clone = NULL;
    tee->queue = queue;
    tee->next = NULL;

    return tee;
}

int
rbh_iter_tee(struct rbh_iterator *iterator, struct rbh_iterator *iterators[2])
{
    struct tee_iterator *tees[2];

    tees[0] = tee_iter_new(iterator);
    if (tees[0] == NULL)
        return -1;

    tees[1] = tee_iter_new(iterator);
    if (tees[1] == NULL) {
        int save_errno = errno;

        rbh_queue_destroy(tees[0]->queue);
        free(tees[0]);

        errno = save_errno;
        return -1;
    }

    /* Link both tee_iterator with each other */
    tees[0]->clone = tees[1];
    tees[1]->clone = tees[0];

    iterators[0] = &tees[0]->iterator;
    iterators[1] = &tees[1]->iterator;
    return 0;
}

/*----------------------------------------------------------------------------*
 |                             rbh_mut_iter_tee()                             |
 *----------------------------------------------------------------------------*/

int
rbh_mut_iter_tee(struct rbh_mut_iterator *iterator,
                 struct rbh_mut_iterator *iterators[2])
{
    return rbh_iter_tee((struct rbh_iterator *)iterator,
                        (struct rbh_iterator **)iterators);
}
