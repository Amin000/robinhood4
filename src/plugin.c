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

#include <dlfcn.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>

#include "robinhood/plugin.h"

static char *
rbh_plugin_library(const char *name)
{
    char *library;

    if (asprintf(&library, "librbh-%s.so", name) < 0) {
        errno = ENOMEM;
        return NULL;
    }
    return library;
}

void *
rbh_plugin_import(const char *name, const char *symbol)
{
    void *dlhandle;
    char *libname;
    void *sym;

    libname = rbh_plugin_library(name);
    if (libname == NULL)
        return NULL;

    dlhandle = dlopen(libname, RTLD_NOW | RTLD_NODELETE | RTLD_LOCAL);
    free(libname);
    if (dlhandle == NULL)
        return NULL;

    dlerror();
    sym = dlsym(dlhandle, symbol);
    dlclose(dlhandle);

    return sym;
}
