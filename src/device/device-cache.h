/*
 * Cord - background device enumeration cache.
 *
 * WHY THIS EXISTS
 *
 * obs_source_info::get_properties runs on the Qt UI thread. Enumerating phones
 * is not cheap: it connects to the adb server, may have to spawn it and wait
 * for it to bind, and waits on socket reads. Doing that inline froze the whole
 * OBS window for seconds every time the source was clicked - Windows showed it
 * as "not responding".
 *
 * So enumeration happens on a poller thread and the UI only ever reads a
 * snapshot from memory. The device list can be up to CORD_CACHE_POLL_MS stale,
 * which costs nothing: a phone that was plugged in a second ago still appears
 * after the next poll, and the Refresh button nudges the poller.
 *
 * The poller starts lazily, on first use, and then runs for the rest of the
 * OBS session - so merely having the plugin installed never leaves an adb
 * server running.
 */
#pragma once

#include "adb-client.h"

#include <stdbool.h>

/* How often the poller re-enumerates. */
#define CORD_CACHE_POLL_MS 2000

void cord_device_cache_init(void);
void cord_device_cache_shutdown(void);

/*
 * Starts the poller if it is not already running. Cheap and idempotent; call it
 * from anywhere that is about to want a device list.
 */
void cord_device_cache_ensure_running(void);

/* Wakes the poller for an immediate refresh (the Refresh button). */
void cord_device_cache_nudge(void);

/*
 * Copies the current snapshot. Never blocks on I/O. Free the result with
 * cord_adb_free_device_list().
 *
 * `wait_ms` only applies before the very first snapshot exists: it waits up to
 * that long for the poller's first pass so the dropdown is not empty the first
 * time it is opened. Once warm, it returns immediately.
 */
void cord_device_cache_get(struct cord_device_list *out, int wait_ms);

/* True once at least one enumeration pass has completed. */
bool cord_device_cache_is_warm(void);
