#include "decoder.h"

#include <util/platform.h>
#include <util/bmem.h>

#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libavutil/pixdesc.h> /* av_get_pix_fmt_name */

#include <string.h>

#define LOG(level, fmt, ...) \
	blog(level, "[cord/video] " fmt, ##__VA_ARGS__)

struct cord_decoder {
	obs_source_t *source;
	struct cord_clock *clock;

	const AVCodec *codec;
	AVCodecContext *ctx;
	AVPacket *packet;
	AVFrame *frame;

	/*
	 * H.264/H.265 send codec configuration as a separate packet. The device
	 * server expects the client to prepend it to the next frame rather than
	 * decode it alone, so it is held here until that frame arrives.
	 */
	uint8_t *pending_config;
	size_t pending_config_size;
	bool merge_config;

	uint64_t frames;
	uint64_t errors;

	/* Logged once rather than per frame, which would flood the log. */
	bool warned_unsupported_format;
};

static enum AVCodecID map_codec_id(uint32_t scrcpy_codec_id)
{
	switch (scrcpy_codec_id) {
	case CORD_CODEC_ID_H264:
		return AV_CODEC_ID_H264;
	case CORD_CODEC_ID_H265:
		return AV_CODEC_ID_HEVC;
	case CORD_CODEC_ID_AV1:
		return AV_CODEC_ID_AV1;
	default:
		return AV_CODEC_ID_NONE;
	}
}

/* FFmpeg pixel format -> libobs video format, for the formats a phone emits. */
static enum video_format map_pixel_format(enum AVPixelFormat pix_fmt,
					  bool *full_range)
{
	*full_range = false;

	switch (pix_fmt) {
	case AV_PIX_FMT_YUV420P:
		return VIDEO_FORMAT_I420;
	case AV_PIX_FMT_YUVJ420P: /* same layout, full range */
		*full_range = true;
		return VIDEO_FORMAT_I420;
	case AV_PIX_FMT_NV12:
		return VIDEO_FORMAT_NV12;
	case AV_PIX_FMT_YUV420P10LE:
		return VIDEO_FORMAT_I010;
	case AV_PIX_FMT_P010LE:
		return VIDEO_FORMAT_P010;
	default:
		return VIDEO_FORMAT_NONE;
	}
}

struct cord_decoder *cord_decoder_create(uint32_t scrcpy_codec_id,
					 obs_source_t *source,
					 struct cord_clock *clock)
{
	const enum AVCodecID av_id = map_codec_id(scrcpy_codec_id);
	if (av_id == AV_CODEC_ID_NONE) {
		LOG(LOG_ERROR, "device offered an unknown video codec (0x%08x)",
		    scrcpy_codec_id);
		return NULL;
	}

	struct cord_decoder *dec = bzalloc(sizeof(struct cord_decoder));
	dec->source = source;
	dec->clock = clock;
	/* Only the Annex-B codecs need the config packet merged. */
	dec->merge_config = (scrcpy_codec_id == CORD_CODEC_ID_H264 ||
			     scrcpy_codec_id == CORD_CODEC_ID_H265);

	dec->codec = avcodec_find_decoder(av_id);
	if (!dec->codec) {
		LOG(LOG_ERROR, "no decoder available for %s",
		    avcodec_get_name(av_id));
		goto fail;
	}

	dec->ctx = avcodec_alloc_context3(dec->codec);
	if (!dec->ctx) {
		LOG(LOG_ERROR, "could not allocate a decoder context");
		goto fail;
	}

	/*
	 * Latency settings. See the header for why each one matters; getting
	 * these wrong is the difference between 30ms and 150ms of lag.
	 */
	dec->ctx->flags |= AV_CODEC_FLAG_LOW_DELAY;
	dec->ctx->flags2 |= AV_CODEC_FLAG2_FAST;
	dec->ctx->thread_type = FF_THREAD_SLICE;
	dec->ctx->thread_count = 0; /* 0 = one per core, within slice mode */
	/* PTS arrives in microseconds. */
	dec->ctx->pkt_timebase = (AVRational){1, 1000000};

	if (avcodec_open2(dec->ctx, dec->codec, NULL) < 0) {
		LOG(LOG_ERROR, "could not open the %s decoder",
		    avcodec_get_name(av_id));
		goto fail;
	}

	dec->packet = av_packet_alloc();
	dec->frame = av_frame_alloc();
	if (!dec->packet || !dec->frame) {
		LOG(LOG_ERROR, "out of memory allocating decoder buffers");
		goto fail;
	}

	LOG(LOG_INFO, "%s decoder ready (software, low-delay, slice-threaded)",
	    avcodec_get_name(av_id));
	return dec;

fail:
	cord_decoder_destroy(dec);
	return NULL;
}

void cord_decoder_destroy(struct cord_decoder *dec)
{
	if (!dec)
		return;

	if (dec->frame)
		av_frame_free(&dec->frame);
	if (dec->packet)
		av_packet_free(&dec->packet);
	if (dec->ctx)
		avcodec_free_context(&dec->ctx);

	bfree(dec->pending_config);
	bfree(dec);
}

/* Pushes one decoded frame to libobs. */
static void emit_frame(struct cord_decoder *dec, AVFrame *frame)
{
	bool full_range = false;
	const enum video_format fmt =
		map_pixel_format((enum AVPixelFormat)frame->format,
				 &full_range);

	if (fmt == VIDEO_FORMAT_NONE) {
		if (!dec->warned_unsupported_format) {
			dec->warned_unsupported_format = true;
			LOG(LOG_ERROR,
			    "device produced an unsupported pixel format (%s)",
			    av_get_pix_fmt_name(
				    (enum AVPixelFormat)frame->format));
		}
		return;
	}

	struct obs_source_frame out;
	memset(&out, 0, sizeof(out));

	out.format = fmt;
	out.width = (uint32_t)frame->width;
	out.height = (uint32_t)frame->height;
	out.full_range = full_range;
	out.flip = false;

	for (int i = 0; i < MAX_AV_PLANES && i < AV_NUM_DATA_POINTERS; i++) {
		out.data[i] = frame->data[i];
		out.linesize[i] = (uint32_t)frame->linesize[i];
	}

	/*
	 * Phone encoders emit BT.709 limited range. Telling libobs the wrong
	 * matrix is the usual cause of washed-out or crushed mirrored video.
	 */
	enum video_colorspace cs = VIDEO_CS_709;
	if (frame->colorspace == AVCOL_SPC_BT470BG ||
	    frame->colorspace == AVCOL_SPC_SMPTE170M)
		cs = VIDEO_CS_601;

	if (frame->color_range == AVCOL_RANGE_JPEG)
		out.full_range = true;

	video_format_get_parameters_for_format(
		cs, out.full_range ? VIDEO_RANGE_FULL : VIDEO_RANGE_PARTIAL,
		fmt, out.color_matrix, out.color_range_min,
		out.color_range_max);

	/*
	 * best_effort_timestamp falls back sensibly when a packet carried no
	 * usable PTS (config packets, or the first frame after a seek-like gap).
	 */
	int64_t pts = frame->best_effort_timestamp;
	if (pts == AV_NOPTS_VALUE)
		pts = frame->pts;

	out.timestamp = (pts == AV_NOPTS_VALUE)
				? os_gettime_ns()
				: cord_clock_to_host_ns(dec->clock,
							(uint64_t)pts);

	obs_source_output_video(dec->source, &out);
	dec->frames++;

	/*
	 * One line, once: proof that the whole chain works end to end. Without
	 * it, "connected" and "actually producing pictures" look identical in
	 * the log, which is the difference between a working capture and a
	 * black source.
	 */
	if (dec->frames == 1) {
		LOG(LOG_INFO,
		    "first frame decoded: %ux%u %s, %s range - video is flowing",
		    out.width, out.height,
		    av_get_pix_fmt_name((enum AVPixelFormat)frame->format),
		    out.full_range ? "full" : "limited");
	}
}

bool cord_decoder_feed(struct cord_decoder *dec,
		       const struct cord_packet_header *hdr,
		       const uint8_t *data)
{
	if (!dec || !dec->ctx)
		return false;

	/* Hold codec configuration back for the next real frame. */
	if (hdr->is_config && dec->merge_config) {
		bfree(dec->pending_config);
		dec->pending_config = bmemdup(data, hdr->size);
		dec->pending_config_size = hdr->size;
		return true;
	}

	const size_t total = dec->pending_config_size + hdr->size;

	if (av_new_packet(dec->packet, (int)total) < 0) {
		LOG(LOG_ERROR, "out of memory for a %zu byte packet", total);
		return false;
	}

	if (dec->pending_config_size) {
		memcpy(dec->packet->data, dec->pending_config,
		       dec->pending_config_size);
		bfree(dec->pending_config);
		dec->pending_config = NULL;
		dec->pending_config_size = 0;
	}

	memcpy(dec->packet->data + (total - hdr->size), data, hdr->size);

	/* Config-only packets carry no meaningful presentation time. */
	dec->packet->pts = hdr->is_config ? AV_NOPTS_VALUE
					  : (int64_t)hdr->pts_us;
	dec->packet->dts = dec->packet->pts;
	if (hdr->is_key_frame)
		dec->packet->flags |= AV_PKT_FLAG_KEY;

	int ret = avcodec_send_packet(dec->ctx, dec->packet);
	av_packet_unref(dec->packet);

	if (ret < 0 && ret != AVERROR(EAGAIN)) {
		dec->errors++;
		/*
		 * A corrupt packet is not fatal: the device sends a key frame
		 * periodically, so the picture recovers on its own. Only log,
		 * and only occasionally, so a bad cable cannot flood the log.
		 */
		if (dec->errors <= 5 || dec->errors % 100 == 0)
			LOG(LOG_WARNING, "decoder rejected a packet (%d), "
					 "%llu so far",
			    ret, (unsigned long long)dec->errors);
		return true;
	}

	/* Drain everything available; low-delay means usually exactly one. */
	for (;;) {
		ret = avcodec_receive_frame(dec->ctx, dec->frame);
		if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
			break;
		if (ret < 0) {
			dec->errors++;
			break;
		}

		emit_frame(dec, dec->frame);
		av_frame_unref(dec->frame);
	}

	return true;
}

void cord_decoder_stats(const struct cord_decoder *dec, uint64_t *frames_out,
			uint64_t *errors_out)
{
	if (!dec)
		return;
	if (frames_out)
		*frames_out = dec->frames;
	if (errors_out)
		*errors_out = dec->errors;
}
