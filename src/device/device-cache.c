#include "device-cache.h"

#include <obs-module.h>
#include <util/platform.h>
#include <util/threading.h>

#include <string.h>

#define LOG(level, fmt, ...) \
	blog(level, "[cord/cache] " fmt, ##__VA_ARGS__)

static struct {
	pthread_mutex_t mutex;
	bool initialised;

	/* The snapshot the UI reads. */
	struct cord_device *items;
	size_t count;
	bool warm;

	pthread_t thread;
	bool thread_running;
	volatile bool stop;
	os_event_t *wake; /* nudge, and the poll interval's timer */
} cache;

static void store_snapshot(const struct cord_device_list *list)
{
	struct cord_device *copy = NULL;
	if (list->count) {
		copy = bmemdup(list->items,
			       sizeof(struct cord_device) * list->count);
	}

	pthread_mutex_lock(&cache.mutex);
	bfree(cache.items);
	cache.items = copy;
	cache.count = list->count;
	cache.warm = true;
	pthread_mutex_unlock(&cache.mutex);
}

static void *poller(void *unused)
{
	UNUSED_PARAMETER(unused);
	os_set_thread_name("cord: device poll");

	size_t last_count = (size_t)-1;

	while (!cache.stop) {
		struct cord_device_list list;
		if (cord_adb_list_devices(&list)) {
			/* Only log when the set changes, not every 2 seconds. */
			if (list.count != last_count) {
				LOG(LOG_INFO, "%llu device(s) connected",
				    (unsigned long long)list.count);
				last_count = list.count;
			}
			store_snapshot(&list);
			cord_adb_free_device_list(&list);
		} else {
			/*
			 * Enumeration failed (no adb at all). Publish an empty
			 * snapshot so the UI says "no phone" rather than
			 * waiting forever for a first result.
			 */
			struct cord_device_list empty = {0};
			store_snapshot(&empty);
			if (last_count != 0) {
				LOG(LOG_WARNING,
				    "could not enumerate devices; is adb present?");
				last_count = 0;
			}
		}

		if (cache.stop)
			break;

		/*
		 * Sleep until the next poll, or until nudged. A manual event
		 * reset would race with a nudge arriving mid-poll, so it is
		 * reset immediately after waking.
		 */
		os_event_timedwait(cache.wake, CORD_CACHE_POLL_MS);
		os_event_reset(cache.wake);
	}

	return NULL;
}

void cord_device_cache_init(void)
{
	if (cache.initialised)
		return;

	memset(&cache, 0, sizeof(cache));
	pthread_mutex_init(&cache.mutex, NULL);
	os_event_init(&cache.wake, OS_EVENT_TYPE_MANUAL);
	cache.initialised = true;
}

void cord_device_cache_shutdown(void)
{
	if (!cache.initialised)
		return;

	cache.stop = true;
	if (cache.wake)
		os_event_signal(cache.wake);

	if (cache.thread_running) {
		pthread_join(cache.thread, NULL);
		cache.thread_running = false;
	}

	if (cache.wake)
		os_event_destroy(cache.wake);

	bfree(cache.items);
	cache.items = NULL;
	cache.count = 0;

	pthread_mutex_destroy(&cache.mutex);
	cache.initialised = false;
}

void cord_device_cache_ensure_running(void)
{
	if (!cache.initialised)
		return;

	pthread_mutex_lock(&cache.mutex);
	const bool start = !cache.thread_running && !cache.stop;
	if (start)
		cache.thread_running = true; /* claim it before unlocking */
	pthread_mutex_unlock(&cache.mutex);

	if (!start)
		return;

	if (pthread_create(&cache.thread, NULL, poller, NULL) != 0) {
		LOG(LOG_ERROR, "could not start the device poller");
		pthread_mutex_lock(&cache.mutex);
		cache.thread_running = false;
		pthread_mutex_unlock(&cache.mutex);
	}
}

void cord_device_cache_nudge(void)
{
	if (cache.initialised && cache.wake)
		os_event_signal(cache.wake);
}

bool cord_device_cache_is_warm(void)
{
	if (!cache.initialised)
		return false;

	pthread_mutex_lock(&cache.mutex);
	const bool warm = cache.warm;
	pthread_mutex_unlock(&cache.mutex);
	return warm;
}

void cord_device_cache_get(struct cord_device_list *out, int wait_ms)
{
	memset(out, 0, sizeof(*out));

	if (!cache.initialised)
		return;

	cord_device_cache_ensure_running();

	/*
	 * Only ever waited on before the first snapshot exists, so the very
	 * first time the dropdown opens it is not empty. Polled in short slices
	 * rather than on a condition variable: this runs on the UI thread, and a
	 * bounded wait that can never exceed wait_ms is the point.
	 */
	for (int waited = 0; waited < wait_ms && !cord_device_cache_is_warm();
	     waited += 50) {
		os_sleep_ms(50);
	}

	pthread_mutex_lock(&cache.mutex);
	if (cache.count) {
		out->items = bmemdup(cache.items, sizeof(struct cord_device) *
							  cache.count);
		out->count = cache.count;
	}
	pthread_mutex_unlock(&cache.mutex);
}
