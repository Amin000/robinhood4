/* This file is part of RobinHood 4
 * Copyright (C) 2022 Commissariat a l'energie atomique et aux energies
 *                    alternatives
 *
 * SPDX-License-Identifer: LGPL-3.0-or-later
 */

#ifndef RBH_FIND_LUSTRE_FILTERS_H
#define RBH_FIND_LUSTRE_FILTERS_H

#include "parser.h"

#include <rbh-find/filters.h>

/**
 * Build a filter for the -hsm-state predicate
 *
 * @param hsm_state    a string representing a HSM state
 *
 * @return             a pointer to a newly allocated struct rbh_filter
 *
 * Exit on error
 */
struct rbh_filter *
hsm_state2filter(const char *hsm_state);

/**
 * Build a filter for the -fid predicate
 *
 * @param fid          a string representing a FID
 *
 * @return             a pointer to a newly allocated struct rbh_filter
 *
 * Exit on error
 */
struct rbh_filter *
fid2filter(const char *fid);

/**
 * Build a filter for the -ost predicate
 *
 * @param ost_index    a string representing a single integer
 *
 * @return             a pointer to a newly allocated struct rbh_filter
 */
struct rbh_filter *
ost_index2filter(const char *ost_index);

/**
 * Build a filter for the -expired predicate
 *
 * @return             a pointer to a newly allocated struct rbh_filter
 */
struct rbh_filter *
expired2filter();

/**
 * Build a filter for the -expired predicate
 *
 * @param expired      the next argument in the command line or NULL
 *
 * @return             a pointer to a newly allocated struct rbh_filter
 */
struct rbh_filter *
expired_at2filter(const char *expired);

#endif
