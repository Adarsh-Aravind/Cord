/*
 * Cord - video decoder.
 *
 * Decodes the device's H.264/H.265/AV1 elementary stream straight into OBS.
 *
 * Why software decode, not D3D11VA:
 *   libobs takes CPU-side frames through obs_source_output_video(), so a
 *   hardware decoder would have to copy every frame back from VRAM. That
 *   readback stalls the GPU pipeline and costs more latency than it saves at
 *   1080p60 H.264, which a single modern core decodes several times faster than
 *   real time. A zero-copy D3D11 texture path is the right optimisation later,
 *   but it belongs in the render callback, not here.
 *
 * Latency rules enforced below:
 *   - slice threading only. FF_THREAD_FRAME pipelines several frames before
 *     emitting the first, which is latency measured in whole frames.
 *   - AV_CODEC_FLAG_LOW_DELAY, so the decoder never holds a frame back for
 *     reordering. The device encodes with no B-frames, so nothing is lost.
 *   - no frame queue: the decoded frame is handed to libobs on the thread that
 *     decoded it.
 */
#pragma once

#include "../device/scrcpy-proto.h"
#include "../util/cord-clock.h"

#include <obs-module.h>

#include <stdbool.h>
#include <stdint.h>

struct cord_decoder;

/*
 * Creates a decoder for a scrcpy codec id (CORD_CODEC_ID_H264 etc).
 * `source` receives the decoded frames. Returns NULL if the codec is not
 * supported by the FFmpeg build in use.
 */
struct cord_decoder *cord_decoder_create(uint32_t scrcpy_codec_id,
					 obs_source_t *source,
					 struct cord_clock *clock);

void cord_decoder_destroy(struct cord_decoder *dec);

/*
 * Feeds one demuxed packet. Config packets (SPS/PPS) are retained and merged
 * into the next frame, matching what the device server expects of a client.
 * Returns false only on an unrecoverable decoder error.
 */
bool cord_decoder_feed(struct cord_decoder *dec,
		       const struct cord_packet_header *hdr,
		       const uint8_t *data);

/* Frames successfully pushed to OBS, and packets dropped by the decoder. */
void cord_decoder_stats(const struct cord_decoder *dec, uint64_t *frames_out,
			uint64_t *errors_out);
