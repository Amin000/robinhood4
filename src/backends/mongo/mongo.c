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
#include <stdlib.h>

#include <sys/stat.h>

/* This backend uses libmongoc, from the "mongo-c-driver" project to interact
 * with a MongoDB database.
 *
 * The documentation for the project can be found at: https://mongoc.org
 */

#include <bson.h>
#include <mongoc.h>

#include "robinhood/backends/mongo.h"
#ifndef HAVE_STATX
# include "robinhood/statx.h"
#endif

#include "mongo.h"

/* libmongoc imposes that mongoc_init() be called before any other mongoc_*
 * function; and mongoc_cleanup() after the last one.
 */

__attribute__((constructor))
static void
mongo_init(void)
{
    mongoc_init();
}

__attribute__((destructor))
static void
mongo_cleanup(void)
{
    mongoc_cleanup();
}

    /*--------------------------------------------------------------------*
     |                     bson_pipeline_from_filter                      |
     *--------------------------------------------------------------------*/

static bson_t *
bson_pipeline_from_filter(const struct rbh_filter *filter)
{
    bson_t *pipeline = bson_new();
    bson_t array;
    bson_t stage;

    if (BSON_APPEND_ARRAY_BEGIN(pipeline, "pipeline", &array)
     && BSON_APPEND_DOCUMENT_BEGIN(&array, "0", &stage)
     && BSON_APPEND_UTF8(&stage, "$unwind", "$" MFF_NAMESPACE)
     && bson_append_document_end(&array, &stage)
     && BSON_APPEND_DOCUMENT_BEGIN(&array, "1", &stage)
     && BSON_APPEND_RBH_FILTER(&stage, "$match", filter)
     && bson_append_document_end(&array, &stage)
     && bson_append_array_end(pipeline, &array))
        return pipeline;

    bson_destroy(pipeline);
    errno = ENOBUFS;
    return NULL;
}

/*----------------------------------------------------------------------------*
 |                               mongo_iterator                               |
 *----------------------------------------------------------------------------*/

struct mongo_iterator {
    struct rbh_mut_iterator iterator;
    mongoc_cursor_t *cursor;
};

static void *
mongo_iter_next(void *iterator)
{
    struct mongo_iterator *mongo_iter = iterator;
    int save_errno = errno;
    const bson_t *doc;

    errno = 0;
    if (mongoc_cursor_next(mongo_iter->cursor, &doc)) {
        errno = save_errno;
        return fsentry_from_bson(doc);
    }

    errno = errno ? : ENODATA;
    return NULL;
}

static void
mongo_iter_destroy(void *iterator)
{
    struct mongo_iterator *mongo_iter = iterator;

    mongoc_cursor_destroy(mongo_iter->cursor);
    free(mongo_iter);
}

static const struct rbh_mut_iterator_operations MONGO_ITER_OPS = {
    .next = mongo_iter_next,
    .destroy = mongo_iter_destroy,
};

static const struct rbh_mut_iterator MONGO_ITER = {
    .ops = &MONGO_ITER_OPS,
};

static struct mongo_iterator *
mongo_iterator_new(mongoc_cursor_t *cursor)
{
    struct mongo_iterator *mongo_iter;

    mongo_iter = malloc(sizeof(*mongo_iter));
    if (mongo_iter == NULL)
        return NULL;

    mongo_iter->iterator = MONGO_ITER;
    mongo_iter->cursor = cursor;

    return mongo_iter;
}

/*----------------------------------------------------------------------------*
 |                             MONGO_BACKEND_OPS                              |
 *----------------------------------------------------------------------------*/

struct mongo_backend {
    struct rbh_backend backend;
    mongoc_uri_t *uri;
    mongoc_client_t *client;
    mongoc_database_t *db;
    mongoc_collection_t *entries;
};

    /*--------------------------------------------------------------------*
     |                               update                               |
     *--------------------------------------------------------------------*/

static mongoc_bulk_operation_t *
_mongoc_collection_create_bulk_operation(
        mongoc_collection_t *collection, bool ordered,
        mongoc_write_concern_t *write_concern
        )
{
#if MONGOC_CHECK_VERSION(1, 9, 0)
    bson_t opts;

    bson_init(&opts);

    if (!BSON_APPEND_BOOL(&opts, "ordered", ordered)) {
        errno = ENOBUFS;
        return NULL;
    }

    if (write_concern && !mongoc_write_concern_append(write_concern, &opts)) {
        errno = EINVAL;
        return NULL;
    }

    return mongoc_collection_create_bulk_operation_with_opts(collection, &opts);
#else
    return mongoc_collection_create_bulk_operation(collection, ordered,
                                                   write_concern);
#endif
}

static bool
_mongoc_bulk_operation_update_one(mongoc_bulk_operation_t *bulk,
                                  const bson_t *selector, const bson_t *update,
                                  bool upsert)
{
#if MONGOC_CHECK_VERSION(1, 7, 0)
    bson_t opts;

    bson_init(&opts);
    if (!BSON_APPEND_BOOL(&opts, "upsert", upsert)) {
        errno = ENOBUFS;
        return NULL;
    }

    /* TODO: handle errors */
    return mongoc_bulk_operation_update_one_with_opts(bulk, selector, update,
                                                      &opts, NULL);
#else
    mongoc_bulk_operation_update_one(bulk, selector, update, upsert);
    return true;
#endif
}

static bool
_mongoc_bulk_operation_remove_one(mongoc_bulk_operation_t *bulk,
                                  const bson_t *selector)
{
#if MONGOC_CHECK_VERSION(1, 7, 0)
    /* TODO: handle errors */
    return mongoc_bulk_operation_remove_one_with_opts(bulk, selector, NULL,
                                                      NULL);
#else
    mongoc_bulk_operation_remove_one(bulk, selector);
    return true;
#endif
}

static bool
mongo_bulk_append_fsevent(mongoc_bulk_operation_t *bulk,
                          const struct rbh_fsevent *fsevent)
{
    bson_t selector;
    bson_t *update;
    bool success;

    bson_init(&selector);
    if (!BSON_APPEND_RBH_ID_FILTER(&selector, MFF_ID, &fsevent->id))
        return false;

    switch (fsevent->type) {
    case RBH_FET_DELETE:
        success = _mongoc_bulk_operation_remove_one(bulk, &selector);
        break;
    case RBH_FET_LINK:
        /* Unlink first, then link */
        update = bson_from_unlink(fsevent->link.parent_id,
                                  fsevent->link.name);
        if (update == NULL)
            return false;

        success = _mongoc_bulk_operation_update_one(bulk, &selector, update,
                                                    false);
        bson_destroy(update);

        if (!success)
            break;
        __attribute__((fallthrough));
    default:
        update = bson_update_from_fsevent(fsevent);
        if (update == NULL)
            return false;

        success = _mongoc_bulk_operation_update_one(
                bulk, &selector, update, fsevent->type != RBH_FET_UNLINK
                );
        bson_destroy(update);
    }

    if (!success) {
        /* > returns false if passed invalid arguments */
        errno = EINVAL;
        return false;
    }

    return true;
}

static const struct rbh_fsevent *
fsevent_iter_next(struct rbh_iterator *fsevents)
{
    const struct rbh_fsevent *fsevent;
    int save_errno = errno;

    errno = 0;
    fsevent = rbh_iter_next(fsevents);
    if (errno == 0 || errno == ENODATA)
        errno = save_errno;
    return fsevent;
}

#define fsevent_for_each(fsevent, fsevents) \
    for (fsevent = fsevent_iter_next(fsevents); fsevent != NULL; \
         fsevent = fsevent_iter_next(fsevents))

static ssize_t
mongo_bulk_init_from_fsevents(mongoc_bulk_operation_t *bulk,
                              struct rbh_iterator *fsevents)
{
    const struct rbh_fsevent *fsevent;
    int save_errno = errno;
    size_t count = 0;

    errno = 0;
    fsevent_for_each(fsevent, fsevents) {
        if (!mongo_bulk_append_fsevent(bulk, fsevent))
            return -1;
        count++;
    }

    if (errno)
        return -1;
    errno = save_errno;

    return count;
}

static ssize_t
mongo_backend_update(void *backend, struct rbh_iterator *fsevents)
{
    struct mongo_backend *mongo = backend;
    mongoc_bulk_operation_t *bulk;
    bson_error_t error;
    ssize_t count;
    bson_t reply;
    uint32_t rc;

    bulk = _mongoc_collection_create_bulk_operation(mongo->entries, false,
                                                    NULL);
    if (bulk == NULL) {
        /* XXX: from libmongoc's documentation:
         *      > "Errors are propagated when executing the bulk operation"
         *
         * We will just assume any error here is related to memory allocation.
         */
        errno = ENOMEM;
        return -1;
    }

    count = mongo_bulk_init_from_fsevents(bulk, fsevents);
    if (count <= 0) {
        int save_errno = errno;

        /* Executing an empty bulk operation is considered an error by mongoc,
         * which is why we return early in this case too
         */
        mongoc_bulk_operation_destroy(bulk);
        errno = save_errno;
        return count;
    }

    rc = mongoc_bulk_operation_execute(bulk, &reply, &error);
    mongoc_bulk_operation_destroy(bulk);
    if (!rc) {
        int errnum = RBH_BACKEND_ERROR;

        snprintf(rbh_backend_error, sizeof(rbh_backend_error), "mongoc: %s",
                 error.message);
#if MONGOC_CHECK_VERSION(1, 11, 0)
        if (mongoc_error_has_label(&reply, "TransientTransactionError"))
            errnum = EAGAIN;
#endif
        bson_destroy(&reply);
        errno = errnum;
        return -1;
    }
    bson_destroy(&reply);

    return count;
}

    /*--------------------------------------------------------------------*
     |                                root                                |
     *--------------------------------------------------------------------*/

static const struct rbh_filter ROOT_FILTER = {
    .op = RBH_FOP_EQUAL,
    .compare = {
        .field = {
            .fsentry = RBH_FP_PARENT_ID,
        },
        .value = {
            .type = RBH_VT_BINARY,
            .binary = {
                .size = 0,
            },
        },
    },
};

static struct rbh_fsentry *
mongo_root(void *backend, unsigned int fsentry_mask, unsigned int statx_mask)
{
    return rbh_backend_filter_one(backend, &ROOT_FILTER, fsentry_mask,
                                  statx_mask);
}

    /*--------------------------------------------------------------------*
     |                          filter fsentries                          |
     *--------------------------------------------------------------------*/

static struct rbh_mut_iterator *
mongo_backend_filter_fsentries(void *backend, const struct rbh_filter *filter,
                               unsigned int fsentry_mask,
                               unsigned int statx_mask)
{
    struct mongo_backend *mongo = backend;
    struct mongo_iterator *mongo_iter;
    mongoc_cursor_t *cursor;
    bson_t *pipeline;

    if (rbh_filter_validate(filter))
        return NULL;

    pipeline = bson_pipeline_from_filter(filter);
    if (pipeline == NULL)
        return NULL;

    cursor = mongoc_collection_aggregate(mongo->entries, MONGOC_QUERY_NONE,
                                         pipeline, NULL, NULL);
    bson_destroy(pipeline);
    if (cursor == NULL) {
        errno = EINVAL;
        return NULL;
    }

    mongo_iter = mongo_iterator_new(cursor);
    if (mongo_iter == NULL) {
        int save_errno = errno;

        mongoc_cursor_destroy(cursor);
        errno = save_errno;
    }

    return &mongo_iter->iterator;
}

    /*--------------------------------------------------------------------*
     |                              destroy                               |
     *--------------------------------------------------------------------*/

static void
mongo_backend_destroy(void *backend)
{
    struct mongo_backend *mongo = backend;

    mongoc_collection_destroy(mongo->entries);
    mongoc_database_destroy(mongo->db);
    mongoc_client_destroy(mongo->client);
    mongoc_uri_destroy(mongo->uri);
    free(mongo);
}

static const struct rbh_backend_operations MONGO_BACKEND_OPS = {
    .root = mongo_root,
    .update = mongo_backend_update,
    .filter_fsentries = mongo_backend_filter_fsentries,
    .destroy = mongo_backend_destroy,
};

/*----------------------------------------------------------------------------*
 |                               MONGO_BACKEND                                |
 *----------------------------------------------------------------------------*/

static const struct rbh_backend MONGO_BACKEND = {
    .id = RBH_BI_MONGO,
    .name = RBH_MONGO_BACKEND_NAME,
    .ops = &MONGO_BACKEND_OPS,
};

/*----------------------------------------------------------------------------*
 |                         rbh_mongo_backend_create()                         |
 *----------------------------------------------------------------------------*/

struct rbh_backend *
rbh_mongo_backend_new(const char *fsname)
{
    struct mongo_backend *mongo;
    int save_errno;

    mongo = malloc(sizeof(*mongo));
    if (mongo == NULL) {
        save_errno = errno;
        goto out;
    }
    mongo->backend = MONGO_BACKEND;

    mongo->uri = mongoc_uri_new("mongodb://localhost:27017");
    if (mongo->uri == NULL) {
        save_errno = EINVAL;
        goto out_free_mongo;
    }

    mongo->client = mongoc_client_new_from_uri(mongo->uri);
    if (mongo->client == NULL) {
        save_errno = ENOMEM;
        goto out_mongoc_uri_destroy;
    }

#if MONGOC_CHECK_VERSION(1, 4, 0)
    if (!mongoc_client_set_error_api(mongo->client,
                                     MONGOC_ERROR_API_VERSION_2)) {
        /* Should never happen */
        save_errno = EINVAL;
        goto out_mongoc_client_destroy;
    }
#endif

    mongo->db = mongoc_client_get_database(mongo->client, fsname);
    if (mongo->db == NULL) {
        save_errno = ENOMEM;
        goto out_mongoc_client_destroy;
    }

    mongo->entries = mongoc_database_get_collection(mongo->db, "entries");
    if (mongo->entries == NULL) {
        save_errno = ENOMEM;
        goto out_mongoc_database_destroy;
    }

    return &mongo->backend;

out_mongoc_database_destroy:
    mongoc_database_destroy(mongo->db);
out_mongoc_client_destroy:
    mongoc_client_destroy(mongo->client);
out_mongoc_uri_destroy:
    mongoc_uri_destroy(mongo->uri);
out_free_mongo:
    free(mongo);
out:
    errno = save_errno;
    return NULL;
}
