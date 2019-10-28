/*
 * lttng-tls-procname.h
 *
 * LTTng UST procname tls.
 *
 * Copyright (C) 2019 Jonathan Rajotte-Julien <jonathan.rajotte-julien@efficios.com>
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
#ifndef _LTTNG_TLS_PROCNAME_H
#define _LTTNG_TLS_PROCNAME_H

/* Reset should not be called from a signal handler. */
void lttng_tls_procname_reset(void);

char* lttng_tls_procname_get(void);

#endif /* _UST_CLOCK_H */
