/*
 * Cord - local monitoring of the phone's audio.
 *
 * WHY THIS EXISTS
 *
 * With the default device-side source ("output"), the phone's audio is captured
 * through REMOTE_SUBMIX, which takes the output mix over - the phone itself
 * goes silent. That is the price of the one source that also carries in-game
 * teammate voice. So while streaming, the only way to hear the game is from the
 * PC.
 *
 * OBS can do that with its monitoring feature, but it is a poor fit here:
 *   - it sits behind the mixer, so it inherits the stream's buffering
 *   - it is a single global device shared with every other monitored source
 *   - pointing it at the device wired to the phone creates the echo loop the
 *     mic-send guard warns about
 *
 * This is a dedicated render path instead: decoded phone audio goes straight to
 * a chosen output device, event-driven, on a Pro Audio thread. It is
 * independent of OBS's audio graph, so monitoring can stay off entirely.
 *
 * HOW IT STAYS CLEAN
 *
 * There is no user-facing buffer setting; the latency is fixed and small (see
 * CORD_MONITOR_*_MS below, roughly 55 ms end to end on the PC side). Getting
 * clean audio out of a network-fed stream is not about the size of the buffer
 * but about never splicing silence into the middle of it:
 *
 *   - Audio arrives from the phone in ~20 ms Opus packets with USB/Wi-Fi
 *     jitter, while the endpoint asks for samples every ~10 ms period. So the
 *     render thread primes a small jitter buffer before it plays anything, and
 *     writes short rather than padding a gap with silence.
 *   - If the stream genuinely dries up, the buffer re-primes once instead of
 *     dribbling a silent gap into every period - one glitch instead of a
 *     continuous crackle.
 *   - The queue is a fixed ring, so neither thread allocates or memmoves while
 *     audio is flowing.
 *   - Rate and channel conversion is handed to WASAPI's own resampler. Only if
 *     the endpoint refuses our format does this fall back to converting in
 *     process, and then it does it with continuous phase across periods.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Latency budget, fixed on purpose.
 *
 * CORD_MONITOR_FILL_MS is how much audio is kept queued inside the endpoint,
 * CORD_MONITOR_CUSHION_MS is the jitter buffer held in front of it. The sum is
 * the added delay. The cushion has to cover more than one 20 ms packet plus
 * scheduling jitter, or the stream underruns and chops.
 */
#define CORD_MONITOR_FILL_MS 20
#define CORD_MONITOR_CUSHION_MS 35

struct cord_audio_player;

/*
 * Opens `device_id` (empty = system default) for playback of interleaved float
 * audio at `sample_rate` / `channels`. Returns NULL on failure, having logged
 * why. The endpoint's own rate and channel count do not have to match.
 */
struct cord_audio_player *cord_audio_player_create(const char *device_id,
						   uint32_t sample_rate,
						   uint32_t channels);

void cord_audio_player_destroy(struct cord_audio_player *player);

/*
 * Queues interleaved float frames. Safe to call from the decode thread; never
 * blocks and never allocates. If the queue has drifted past its target the
 * oldest audio is dropped, because falling behind is worse than a brief glitch
 * for live monitoring.
 */
void cord_audio_player_push(struct cord_audio_player *player,
			    const float *interleaved, uint32_t frames);

/* Linear gain, 1.0 = unity. Applied with a short ramp, so it never clicks. */
void cord_audio_player_set_volume(struct cord_audio_player *player,
				  float volume);

bool cord_audio_player_is_running(const struct cord_audio_player *player);

#ifdef __cplusplus
}
#endif
