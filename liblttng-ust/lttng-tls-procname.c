/*
 * lttng-tls-procname.c
 *
 * LTTng UST procname tls.
 *
 * Copyright (C) 2009-2012 Mathieu Desnoyers <mathieu.desnoyers@efficios.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; only
 * version 2.1 of the License.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#define _LGPL_SOURCE
#include <lttng/ust-events.h>
#include <urcu/tls-compat.h>
#include <assert.h>
#include "compat.h"

/* Maximum number of nesting levels for the procname cache. */
#define PROCNAME_NESTING_MAX	2

/*
 * We cache the result to ensure we don't trigger a system call for
 * each access.
 * Upon exec, procname changes, but exec takes care of throwing away
 * this cached version.
 * The procname can also change by calling prctl(). The procname should
 * be set for a thread before the first event is logged within this
 * thread.
 */
typedef char procname_array[PROCNAME_NESTING_MAX][17];

static DEFINE_URCU_TLS(procname_array, cached_procname);

static DEFINE_URCU_TLS(int, procname_nesting);

static inline
char *wrapper_getprocname(void)
{
	int nesting = CMM_LOAD_SHARED(URCU_TLS(procname_nesting));

	if (caa_unlikely(nesting >= PROCNAME_NESTING_MAX))
		return "<unknown>";
	if (caa_unlikely(!URCU_TLS(cached_procname)[nesting][0])) {
		CMM_STORE_SHARED(URCU_TLS(procname_nesting), nesting + 1);
		/* Increment nesting before updating cache. */
		cmm_barrier();
		lttng_ust_getprocname(URCU_TLS(cached_procname)[nesting]);
		URCU_TLS(cached_procname)[nesting][LTTNG_UST_PROCNAME_LEN - 1] = '\0';
		/* Decrement nesting after updating cache. */
		cmm_barrier();
		CMM_STORE_SHARED(URCU_TLS(procname_nesting), nesting);
	}
	return URCU_TLS(cached_procname)[nesting];
}

/* Reset should not be called from a signal handler. */
void lttng_tls_procname_reset(void)
{
	CMM_STORE_SHARED(URCU_TLS(cached_procname)[1][0], '\0');
	CMM_STORE_SHARED(URCU_TLS(procname_nesting), 1);
	CMM_STORE_SHARED(URCU_TLS(cached_procname)[0][0], '\0');
	CMM_STORE_SHARED(URCU_TLS(procname_nesting), 0);
}

char* lttng_tls_procname_get(void)
{
	return wrapper_getprocname();
}

/*
 * Force a read (imply TLS fixup for dlopen) of TLS variables.
 */
void lttng_fixup_procname_tls(void)
{
	asm volatile ("" : : "m" (URCU_TLS(cached_procname)[0]));
}
