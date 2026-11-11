#include "audio-decode.h"
#include "wasapi-player.h"

#include <util/platform.h>
#include <util/bmem.h>

#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libavutil/samplefmt.h> /* av_get_sample_fmt_name */

#include <string.h>

#define LOG(level, fmt, ...) \
	blog(level, "[cord/audio] " fmt, ##__VA_ARGS__)

struct cord_audio_decoder {
	obs_source_t *source;
	struct cord_clock *clock;

	const AVCodec *codec;
	AVCodecContext *ctx;
	AVPacket *packet;
	AVFrame *frame;

	/* "raw" needs no decoder: the device already sends PCM s16le. */
	bool passthrough_pcm;

	uint64_t frames;
	uint64_t errors;
	bool warned_unsupported_format;

	/* Optional local monitor; not owned. See cord_audio_decoder_set_player. */
	struct cord_audio_player *player;
	/* Scratch for the planar -> interleaved conversion the player wants. */
	float *interleaved;
	size_t interleaved_cap;
};

void cord_audio_decoder_set_player(struct cord_audio_decoder *dec,
				   struct cord_audio_player *player)
{
	if (dec)
		dec->player = player;
}

/*
 * The player takes interleaved float; FFmpeg hands us planar float for Opus and
 * AAC. Converts into a reusable scratch buffer and queues it.
 */
static void feed_player(struct cord_audio_decoder *dec, const AVFrame *frame)
{
	if (!dec->player)
		return;

	const uint32_t frames = (uint32_t)frame->nb_samples;
	const uint32_t channels = CORD_AUDIO_CHANNELS;
	const size_t needed = (size_t)frames * channels;

	if (needed > dec->interleaved_cap) {
		dec->interleaved = brealloc(dec->interleaved,
					    needed * sizeof(float));
		dec->interleaved_cap = needed;
	}

	const enum AVSampleFormat fmt = (enum AVSampleFormat)frame->format;

	if (fmt == AV_SAMPLE_FMT_FLTP) {
		const float *l = (const float *)frame->data[0];
		const float *r = frame->data[1] ? (const float *)frame->data[1]
						: l;
		for (uint32_t i = 0; i < frames; i++) {
			dec->interleaved[i * 2] = l[i];
			dec->interleaved[i * 2 + 1] = r[i];
		}
	} else if (fmt == AV_SAMPLE_FMT_FLT) {
		memcpy(dec->interleaved, frame->data[0],
		       needed * sizeof(float));
	} else if (fmt == AV_SAMPLE_FMT_S16) {
		const int16_t *s = (const int16_t *)frame->data[0];
		for (size_t i = 0; i < needed; i++)
			dec->interleaved[i] = s[i] / 32768.0f;
	} else if (fmt == AV_SAMPLE_FMT_S16P) {
		const int16_t *l = (const int16_t *)frame->data[0];
		const int16_t *r = frame->data[1]
					   ? (const int16_t *)frame->data[1]
					   : l;
		for (uint32_t i = 0; i < frames; i++) {
			dec->interleaved[i * 2] = l[i] / 32768.0f;
			dec->interleaved[i * 2 + 1] = r[i] / 32768.0f;
		}
	} else {
		return; /* already reported by the OBS path */
	}

	cord_audio_player_push(dec->player, dec->interleaved, frames);
}

static enum AVCodecID map_codec_id(uint32_t scrcpy_codec_id)
{
	switch (scrcpy_codec_id) {
	case CORD_CODEC_ID_OPUS:
		return AV_CODEC_ID_OPUS;
	case CORD_CODEC_ID_AAC:
		return AV_CODEC_ID_AAC;
	case CORD_CODEC_ID_FLAC:
		return AV_CODEC_ID_FLAC;
	case CORD_CODEC_ID_RAW:
		return AV_CODEC_ID_PCM_S16LE;
	default:
		return AV_CODEC_ID_NONE;
	}
}

static enum audio_format map_sample_format(enum AVSampleFormat fmt)
{
	switch (fmt) {
	case AV_SAMPLE_FMT_FLTP:
		return AUDIO_FORMAT_FLOAT_PLANAR;
	case AV_SAMPLE_FMT_FLT:
		return AUDIO_FORMAT_FLOAT;
	case AV_SAMPLE_FMT_S16P:
		return AUDIO_FORMAT_16BIT_PLANAR;
	case AV_SAMPLE_FMT_S16:
		return AUDIO_FORMAT_16BIT;
	case AV_SAMPLE_FMT_S32P:
		return AUDIO_FORMAT_32BIT_PLANAR;
	case AV_SAMPLE_FMT_S32:
		return AUDIO_FORMAT_32BIT;
	default:
		return AUDIO_FORMAT_UNKNOWN;
	}
}

struct cord_audio_decoder *cord_audio_decoder_create(uint32_t scrcpy_codec_id,
						     obs_source_t *source,
						     struct cord_clock *clock)
{
	const enum AVCodecID av_id = map_codec_id(scrcpy_codec_id);
	if (av_id == AV_CODEC_ID_NONE) {
		LOG(LOG_ERROR, "device offered an unknown audio codec (0x%08x)",
		    scrcpy_codec_id);
		return NULL;
	}

	struct cord_audio_decoder *dec =
		bzalloc(sizeof(struct cord_audio_decoder));
	dec->source = source;
	dec->clock = clock;

	if (scrcpy_codec_id == CORD_CODEC_ID_RAW) {
		/*
		 * Interleaved s16le on the wire already; running it through a
		 * decoder would only add a copy.
		 */
		dec->passthrough_pcm = true;
		LOG(LOG_INFO, "raw PCM audio, no decoding needed");
		return dec;
	}

	dec->codec = avcodec_find_decoder(av_id);
	if (!dec->codec) {
		LOG(LOG_ERROR, "no decoder available for %s",
		    avcodec_get_name(av_id));
		goto fail;
	}

	dec->ctx = avcodec_alloc_context3(dec->codec);
	if (!dec->ctx) {
		LOG(LOG_ERROR, "could not allocate an audio decoder context");
		goto fail;
	}

	/*
	 * The device server does not negotiate: it always sends stereo 48 kHz
	 * and sends its codec config as a PTS-less packet we skip. Setting the
	 * layout and rate here is what lets the Opus decoder open without
	 * extradata.
	 */
	av_channel_layout_default(&dec->ctx->ch_layout, CORD_AUDIO_CHANNELS);
	dec->ctx->sample_rate = CORD_AUDIO_SAMPLE_RATE;
	dec->ctx->pkt_timebase = (AVRational){1, 1000000};

	if (av_id == AV_CODEC_ID_FLAC) {
		/* The FLAC decoder does not advertise a sample format. */
		dec->ctx->sample_fmt = AV_SAMPLE_FMT_S16;
	}

	if (avcodec_open2(dec->ctx, dec->codec, NULL) < 0) {
		LOG(LOG_ERROR, "could not open the %s decoder",
		    avcodec_get_name(av_id));
		goto fail;
	}

	dec->packet = av_packet_alloc();
	dec->frame = av_frame_alloc();
	if (!dec->packet || !dec->frame) {
		LOG(LOG_ERROR, "out of memory allocating audio buffers");
		goto fail;
	}

	LOG(LOG_INFO, "%s decoder ready (%d Hz, %d ch)",
	    avcodec_get_name(av_id), CORD_AUDIO_SAMPLE_RATE,
	    CORD_AUDIO_CHANNELS);
	return dec;

fail:
	cord_audio_decoder_destroy(dec);
	return NULL;
}

void cord_audio_decoder_destroy(struct cord_audio_decoder *dec)
{
	if (!dec)
		return;

	if (dec->frame)
		av_frame_free(&dec->frame);
	if (dec->packet)
		av_packet_free(&dec->packet);
	if (dec->ctx)
		avcodec_free_context(&dec->ctx);

	bfree(dec->interleaved);
	bfree(dec);
}

static void emit_frame(struct cord_audio_decoder *dec, const AVFrame *frame)
{
	const enum audio_format fmt =
		map_sample_format((enum AVSampleFormat)frame->format);

	if (fmt == AUDIO_FORMAT_UNKNOWN) {
		if (!dec->warned_unsupported_format) {
			dec->warned_unsupported_format = true;
			LOG(LOG_ERROR,
			    "decoder produced an unsupported sample format (%s)",
			    av_get_sample_fmt_name(
				    (enum AVSampleFormat)frame->format));
		}
		return;
	}

	struct obs_source_audio out;
	memset(&out, 0, sizeof(out));

	out.format = fmt;
	out.speakers = SPEAKERS_STEREO;
	out.samples_per_sec = (uint32_t)frame->sample_rate;
	out.frames = (uint32_t)frame->nb_samples;

	/* obs_source_audio::data is declared with MAX_AV_PLANES entries. */
	for (int i = 0; i < MAX_AV_PLANES && i < AV_NUM_DATA_POINTERS; i++)
		out.data[i] = frame->data[i];

	int64_t pts = frame->best_effort_timestamp;
	if (pts == AV_NOPTS_VALUE)
		pts = frame->pts;

	out.timestamp = (pts == AV_NOPTS_VALUE)
				? os_gettime_ns()
				: cord_clock_to_host_ns(dec->clock,
							(uint64_t)pts);

	obs_source_output_audio(dec->source, &out);
	feed_player(dec, frame);
	dec->frames++;

	if (dec->frames == 1) {
		LOG(LOG_INFO,
		    "first audio delivered: %u Hz, %u frames, %s - audio is flowing",
		    out.samples_per_sec, out.frames,
		    av_get_sample_fmt_name((enum AVSampleFormat)frame->format));
	}
}

bool cord_audio_decoder_feed(struct cord_audio_decoder *dec,
			     const struct cord_packet_header *hdr,
			     const uint8_t *data)
{
	if (!dec)
		return false;

	/*
	 * Codec config carries no PTS and is not audio. The reference client
	 * drops these too; the decoder was configured up front instead.
	 */
	if (hdr->is_config)
		return true;

	if (dec->passthrough_pcm) {
		const uint32_t bytes_per_frame =
			sizeof(int16_t) * CORD_AUDIO_CHANNELS;
		if (hdr->size < bytes_per_frame)
			return true;

		struct obs_source_audio out;
		memset(&out, 0, sizeof(out));
		out.format = AUDIO_FORMAT_16BIT;
		out.speakers = SPEAKERS_STEREO;
		out.samples_per_sec = CORD_AUDIO_SAMPLE_RATE;
		out.frames = hdr->size / bytes_per_frame;
		out.data[0] = data;
		out.timestamp = cord_clock_to_host_ns(dec->clock, hdr->pts_us);

		obs_source_output_audio(dec->source, &out);
		dec->frames++;
		return true;
	}

	if (av_new_packet(dec->packet, (int)hdr->size) < 0) {
		LOG(LOG_ERROR, "out of memory for a %u byte audio packet",
		    hdr->size);
		return false;
	}

	memcpy(dec->packet->data, data, hdr->size);
	dec->packet->pts = (int64_t)hdr->pts_us;
	dec->packet->dts = dec->packet->pts;

	int ret = avcodec_send_packet(dec->ctx, dec->packet);
	av_packet_unref(dec->packet);

	if (ret < 0 && ret != AVERROR(EAGAIN)) {
		dec->errors++;
		/*
		 * Audio glitches are self-healing - Opus frames are
		 * independent - so a bad packet is not worth tearing the
		 * session down. Rate-limit the logging.
		 */
		if (dec->errors <= 5 || dec->errors % 500 == 0)
			LOG(LOG_WARNING,
			    "audio decoder rejected a packet (%d), %llu so far",
			    ret, (unsigned long long)dec->errors);
		return true;
	}

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

void cord_audio_decoder_stats(const struct cord_audio_decoder *dec,
			      uint64_t *frames_out, uint64_t *errors_out)
{
	if (!dec)
		return;
	if (frames_out)
		*frames_out = dec->frames;
	if (errors_out)
		*errors_out = dec->errors;
}
