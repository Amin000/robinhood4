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

#include <ctype.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "robinhood/uri.h"

/* URI generic syntax: scheme:[//authority]path[?query][#fragment]
 *
 * where authority is: [userinfo@]host[:port]
 *
 * where userinfo is: username[:password]
 *
 * cf. RFC 3986 for more information
 */

int
rbh_parse_raw_uri(struct rbh_raw_uri *uri, char *string)
{
    char *at;

    /* string = scheme:[[//authority]path[?query][#fragment]
     *
     * where scheme = ALPHA *( ALPHA / DIGIT / "+" / "-" / "." )
     */
    if (!isalpha(*string)) {
        errno = EINVAL;
        return -1;
    }

    memset(uri, 0, sizeof(*uri));

    uri->scheme = string;
    do {
        string++;
    } while (isalnum(*string) || *string == '+' || *string == '-'
                || *string == '.');

    if (*string != ':') {
        errno = EINVAL;
        return -1;
    }
    *string++ = '\0';

    /* string = [//authority]path[?query][#fragment] */
    uri->fragment = strrchr(string, '#');
    if (uri->fragment)
        *uri->fragment++ = '\0';

    /* string = [//authority]path[?query] */
    uri->query = strrchr(string, '?');
    if (uri->query)
        *uri->query++ = '\0';

    /* string = [//authority]path */
    if (string[0] != '/' || string[1] != '/') {
        /* string = path */
        uri->path = string;
        return 0;
    }

    /* string = //[userinfo@]host[:port]path
     *
     * where path is either empty or starts with a '/'
     */
    uri->path = strchrnul(string + 2, '/');
    if (uri->path == string + 2)
        /* authority is empty */
        return 0;

    /* Move everything to the left of path, two chars to the left
     * (overwriting the leading "//") so [userinfo@]host[:port] can be
     * separated from path with a '\0' (actually 2 of them).
     */
    memmove(string, string + 2, uri->path - string - 2);
    memset((char *)uri->path - 2, '\0', 2);

    /* string = [userinfo@]host[:port] */
    at = strchr(string, '@');
    if (at) {
        /* string = userinfo@host[:port] */
        uri->userinfo = string;
        *at++ = '\0';
        string = at;
    }

    /* string = host[:port] */
    uri->port = strrchr(string, ':');
    if (uri->port)
        *uri->port++ = '\0';

    /* string = host */
    uri->host = *string == '\0' ? NULL : string;

    return 0;
}
