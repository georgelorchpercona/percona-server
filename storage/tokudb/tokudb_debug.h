/* -*- mode: C++; c-basic-offset: 4; indent-tabs-mode: nil -*- */
// vim: ft=cpp:expandtab:ts=8:sw=4:softtabstop=4:
/* -*- mode: C; c-basic-offset: 4 -*- */
#ident "$Id$"
/*======
This file is part of TokuDB


Copyright (c) 2006, 2015, Percona and/or its affiliates. All rights reserved.

    TokuDBis is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License, version 2,
    as published by the Free Software Foundation.

    TokuDB is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with TokuDB.  If not, see <http://www.gnu.org/licenses/>.

======= */

#ident "Copyright (c) 2006, 2015, Percona and/or its affiliates. All rights reserved."

#ifndef _TOKUDB_DEBUG_H
#define _TOKUDB_DEBUG_H

#include "hatoku_defines.h"

#define TOKU_INCLUDE_BACKTRACE 0
#if TOKU_INCLUDE_BACKTRACE
static void tokudb_backtrace(void);
#endif

// tokudb debug tracing for tokudb_debug declared in tokudb_sysvars.h/.cc
extern ulong tokudb_debug;
#define TOKUDB_DEBUG_INIT                   (1<<0)
#define TOKUDB_DEBUG_OPEN                   (1<<1)
#define TOKUDB_DEBUG_ENTER                  (1<<2)
#define TOKUDB_DEBUG_RETURN                 (1<<3)
#define TOKUDB_DEBUG_ERROR                  (1<<4)
#define TOKUDB_DEBUG_TXN                    (1<<5)
#define TOKUDB_DEBUG_AUTO_INCREMENT         (1<<6)
#define TOKUDB_DEBUG_INDEX_KEY              (1<<7)
#define TOKUDB_DEBUG_LOCK                   (1<<8)
// GL : 9 missing? Why?
#define TOKUDB_DEBUG_CHECK_KEY              (1<<10)
#define TOKUDB_DEBUG_HIDE_DDL_LOCK_ERRORS   (1<<11)
#define TOKUDB_DEBUG_ALTER_TABLE            (1<<12)
#define TOKUDB_DEBUG_UPSERT                 (1<<13)
#define TOKUDB_DEBUG_CHECK                  (1<<14)
#define TOKUDB_DEBUG_ANALYZE                (1<<15)

#define TOKUDB_TRACE(f, ...) { \
    fprintf(stderr, "%u %s:%u %s " f "\n", tokudb::thread::my_tid(), \
            __FILE__, __LINE__, __FUNCTION__, ##__VA_ARGS__); \
}

#define TOKUDB_DBUG_ENTER(f, ...) { \
    if (tokudb_debug & TOKUDB_DEBUG_ENTER) { \
        TOKUDB_TRACE(f, ##__VA_ARGS__);       \
    } \
} \
    DBUG_ENTER(__FUNCTION__);

#define TOKUDB_DBUG_RETURN(r) { \
    int rr = (r); \
    if ((tokudb_debug & TOKUDB_DEBUG_RETURN) || (rr != 0 && (tokudb_debug & TOKUDB_DEBUG_ERROR))) { \
        TOKUDB_TRACE("return %d", rr); \
    } \
    DBUG_RETURN(rr); \
}

#define TOKUDB_HANDLER_TRACE(f, ...) \
    fprintf(stderr, "%u %p %s:%u ha_tokudb::%s " f "\n", tokudb::thread::my_tid(), this, __FILE__, __LINE__, __FUNCTION__, ##__VA_ARGS__);

#define TOKUDB_HANDLER_DBUG_ENTER(f, ...) { \
    if (tokudb_debug & TOKUDB_DEBUG_ENTER) { \
        TOKUDB_HANDLER_TRACE(f, ##__VA_ARGS__); \
    } \
} \
    DBUG_ENTER(__FUNCTION__);

#define TOKUDB_HANDLER_DBUG_RETURN(r) { \
    int rr = (r); \
    if ((tokudb_debug & TOKUDB_DEBUG_RETURN) || (rr != 0 && (tokudb_debug & TOKUDB_DEBUG_ERROR))) { \
        TOKUDB_HANDLER_TRACE("return %d", rr); \
    } \
    DBUG_RETURN(rr); \
}

#define TOKUDB_HANDLER_DBUG_VOID_RETURN { \
    if (tokudb_debug & TOKUDB_DEBUG_RETURN) { \
        TOKUDB_HANDLER_TRACE("return");       \
    } \
    DBUG_VOID_RETURN; \
}

#define TOKUDB_DBUG_DUMP(s, p, len) \
{ \
    TOKUDB_TRACE("%s", s); \
    uint i;                                                             \
    for (i=0; i<len; i++) {                                             \
        fprintf(stderr, "%2.2x", ((uchar*)p)[i]);                       \
    }                                                                   \
    fprintf(stderr, "\n");                                              \
}

/* The purpose of this file is to define assert() for use by the handlerton.
 * The intention is for a failed handlerton assert to invoke a failed assert
 * in the fractal tree layer, which dumps engine status to the error log.
 */

void toku_hton_assert_fail(const char*/*expr_as_string*/,const char */*fun*/,const char*/*file*/,int/*line*/, int/*errno*/) __attribute__((__visibility__("default"))) __attribute__((__noreturn__));

#define assert_always(expr)      ((expr)      ? (void)0 : toku_hton_assert_fail(#expr, __FUNCTION__, __FILE__, __LINE__, errno))

#undef assert
#define assert(expr) assert_always(expr)

#ifdef TOKUDB_DEBUG
    #define assert_debug(expr)       ((expr)      ? (void)0 : toku_hton_assert_fail(#expr, __FUNCTION__, __FILE__, __LINE__, errno))
#else
    #define assert_debug(expr)       (void)0
#endif // TOKUDB_DEBUG

#endif // _TOKUDB_DEBUG_H
