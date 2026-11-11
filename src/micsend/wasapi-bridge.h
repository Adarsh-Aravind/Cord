/*
 * Cord - PC microphone -> phone audio bridge.
 *
 * WHY THIS IS A WASAPI LOOP AND NOT SOFTWARE ROUTING
 *
 * Android exposes no way to inject audio into another app's microphone input.
 * There is no virtual-mic API, ADB cannot feed one, and MediaProjection only
 * goes the other way. On an unrooted phone the only path that works is analog:
 *
 *   PC output -> line-to-mic attenuator -> USB-C audio adapter with a TRRS mic
 *   input -> phone, which treats it as a headset mic.
 *
 * See docs/mic-wiring.md for the parts. The software half is this file: take a
 * PC capture device, apply gain, and render it to the output device that is
 * wired to the phone, at low latency and independently of OBS's own audio
 * graph.
 *
 * Why not just use OBS monitoring: OBS can already route a source to a
 * monitoring device, but monitoring sits behind the mixer and adds buffering,
 * it is a single global device shared with every other monitored source, and it
 * stops when OBS is idle. Voice going to teammates should not inherit the
 * stream's buffering, so this is a dedicated path: ~10ms periods, event-driven,
 * no mixer in between.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct cord_audio_device {
	char id[512];   /* WASAPI endpoint id, used to reopen the device */
	char name[256]; /* what to show the user */
	bool is_default;
};

struct cord_audio_device_list {
	struct cord_audio_device *items;
	size_t count;
};

/* Enumerate active endpoints. Free with cord_audio_device_list_free(). */
bool cord_list_capture_devices(struct cord_audio_device_list *out);
bool cord_list_render_devices(struct cord_audio_device_list *out);
void cord_audio_device_list_free(struct cord_audio_device_list *list);

struct cord_micsend;

/*
 * Starts the bridge. Empty ids mean the system default endpoint.
 * Returns NULL on failure, having logged why.
 */
struct cord_micsend *cord_micsend_create(const char *capture_id,
					 const char *render_id);

void cord_micsend_destroy(struct cord_micsend *ms);

/* Linear gain, 1.0 = unity. Applied before the render client. */
void cord_micsend_set_gain(struct cord_micsend *ms, float gain);

/* Muted means silence is still rendered, keeping the stream running. */
void cord_micsend_set_muted(struct cord_micsend *ms, bool muted);

/* Peak level of the last block, 0..1, for the UI meter. */
float cord_micsend_get_peak(struct cord_micsend *ms);

/* False once the device has been lost (unplugged, format changed). */
bool cord_micsend_is_running(const struct cord_micsend *ms);

/*
 * Plays a 1 kHz tone for `ms_duration` into the render device, so the wiring
 * can be verified before going live: if the phone's voice chat hears this, the
 * cable and the attenuator are right.
 */
void cord_micsend_test_tone(struct cord_micsend *ms, int ms_duration);

#ifdef __cplusplus
}
#endif
