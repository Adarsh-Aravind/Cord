/*
 * Cord - shared presentation clock.
 *
 * The device stamps both streams with PTS values in microseconds from its own
 * monotonic clock. OBS wants nanoseconds on the host clock. Converting each
 * stream independently would let them drift apart, so video and audio share one
 * anchor: the host time at which the first packet of EITHER stream arrived,
 * paired with that packet's device PTS.
 *
 *   host_ns = anchor_host_ns + (pts_us - anchor_pts_us) * 1000
 *
 * This keeps the device's own A/V relationship intact - which is what actually
 * matters, since the phone mixed the audio against the frames it encoded -
 * while staying on a timebase OBS understands.
 *
 * Deliberately NOT re-anchored over time: re-anchoring is a step change in the
 * timeline, which OBS resolves by dropping or repeating, i.e. a visible hitch.
 * Device and host clocks are both monotonic and drift by microseconds over a
 * stream, far below anything a viewer could see.
 */
#pragma once

#include <util/platform.h>
#include <util/threading.h>

#include <stdbool.h>
#include <stdint.h>

struct cord_clock {
	volatile bool anchored;
	uint64_t anchor_host_ns;
	uint64_t anchor_pts_us;
	pthread_mutex_t mutex;
};

static inline void cord_clock_init(struct cord_clock *c)
{
	c->anchored = false;
	c->anchor_host_ns = 0;
	c->anchor_pts_us = 0;
	pthread_mutex_init(&c->mutex, NULL);
}

static inline void cord_clock_free(struct cord_clock *c)
{
	pthread_mutex_destroy(&c->mutex);
}

/* Drops the anchor so the next packet re-establishes it (use on reconnect). */
static inline void cord_clock_reset(struct cord_clock *c)
{
	pthread_mutex_lock(&c->mutex);
	c->anchored = false;
	pthread_mutex_unlock(&c->mutex);
}

/* Converts a device PTS to a host timestamp, anchoring on first use. */
static inline uint64_t cord_clock_to_host_ns(struct cord_clock *c,
					     uint64_t pts_us)
{
	pthread_mutex_lock(&c->mutex);

	if (!c->anchored) {
		c->anchor_host_ns = os_gettime_ns();
		c->anchor_pts_us = pts_us;
		c->anchored = true;
		pthread_mutex_unlock(&c->mutex);
		return c->anchor_host_ns;
	}

	const uint64_t anchor_host = c->anchor_host_ns;
	const uint64_t anchor_pts = c->anchor_pts_us;
	pthread_mutex_unlock(&c->mutex);

	/*
	 * Signed arithmetic: audio can legitimately present slightly before the
	 * stream that anchored the clock, which would wrap an unsigned result
	 * into the far future and stall the source.
	 */
	const int64_t delta_us = (int64_t)pts_us - (int64_t)anchor_pts;
	const int64_t host = (int64_t)anchor_host + delta_us * 1000;

	return host > 0 ? (uint64_t)host : 0;
}
