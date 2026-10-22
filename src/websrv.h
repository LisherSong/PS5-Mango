/* Copyright (C) 2024 John Törnblom

This program is free software; you can redistribute it and/or modify it
under the terms of the GNU General Public License as published by the
Free Software Foundation; either version 3, or (at your option) any
later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; see the file COPYING. If not, see
<http://www.gnu.org/licenses/>.  */

#pragma once

#include <microhttpd.h>

enum MHD_Result websrv_queue_response(struct MHD_Connection *conn,
				      unsigned int status,
				      struct MHD_Response *resp);

int websrv_listen(unsigned short port);
void websrv_stop(void);
int websrv_stop_requested(void);

/* The port this service is listening on (0 before listen). The loopback PKG
 * stream (/stream/install/, src/pkg_stream.c) builds its uri from it: the
 * stream is a route inside this daemon, not a second listener. */
unsigned websrv_service_port(void);

/* Park the service (2026-10-11): after an unrecoverable wedge (a kernel call
 * that never returned), the process stops serving but STAYS ALIVE, so the
 * payload manager does not re-inject a new instance over the wedged one.
 * The accept loop unwinds within ~5ms, the listen port is released, and
 * main() idles in sleep() -- a state SIGKILL can still reap cleanly.
 * Parking is terminal: there is no unpark; the only recovery is a fresh
 * process (a console reboot being the cleanest one). */
void websrv_park(const char *reason);
int websrv_parked(void);

/* Crash-path snapshot of the request ledger (async-signal context: try-lock,
 * no allocation). Writes human-readable lines into out; the last entry
 * without a 'D' mark is the request that was in flight when the process
 * died. See websrv.c's request ledger block for the ring layout. */
void websrv_diag_recent(char *out, size_t n);
