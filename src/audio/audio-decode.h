/*
 * Cord - phone audio decoder.
 *
 * Decodes the device's audio stream and hands it to libobs as a source audio
 * feed, so it lands on whichever OBS tracks the user assigned in Advanced Audio
 * Properties.
 *
 * What this stream actually contains:
 *   With the default device-side source ("output"), this is the phone's entire
 *   output mix - game audio AND teammate voice chat together - captured through
 *   REMOTE_SUBMIX, which is available because the device server runs as
 *   uid=shell. They cannot be separated: an in-game voice chat is mixed inside
 *   the game process before it ever reaches the audio HAL, so no API on any
 *   platform could split them.
 *
 * Stream properties are fixed by the device server, not negotiated: stereo,
 * 48 kHz. The decoder context is configured with those up front, which is also
 * what lets the Opus decoder work without extradata - the device sends its
 * codec config as a separate packet that carries no PTS, and those are skipped
 * exactly as the reference client does.
 */
#pragma once

#include "../device/scrcpy-proto.h"
#include "../util/cord-clock.h"

#include <obs-module.h>

#include <stdbool.h>
#include <stdint.h>

/* Fixed by the device server; see Demuxer's hardcoded audio properties. */
#define CORD_AUDIO_SAMPLE_RATE 48000
#define CORD_AUDIO_CHANNELS 2

struct cord_audio_decoder;
struct cord_audio_player;

struct cord_audio_decoder *cord_audio_decoder_create(uint32_t scrcpy_codec_id,
						     obs_source_t *source,
						     struct cord_clock *clock);

void cord_audio_decoder_destroy(struct cord_audio_decoder *dec);

bool cord_audio_decoder_feed(struct cord_audio_decoder *dec,
			     const struct cord_packet_header *hdr,
			     const uint8_t *data);

/*
 * Attaches a local monitor. Decoded audio is then also written straight to that
 * playback device, so the phone's sound can be heard without OBS monitoring -
 * which matters because the default capture source silences the phone itself.
 * Pass NULL to detach. The decoder does not own the player.
 */
void cord_audio_decoder_set_player(struct cord_audio_decoder *dec,
				   struct cord_audio_player *player);

void cord_audio_decoder_stats(const struct cord_audio_decoder *dec,
			      uint64_t *frames_out, uint64_t *errors_out);
