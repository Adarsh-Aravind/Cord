#include "scrcpy-proto.h"

const char *cord_audio_source_name(enum cord_audio_source src)
{
	switch (src) {
	case CORD_AUDIO_SOURCE_OUTPUT:
		return "output";
	case CORD_AUDIO_SOURCE_PLAYBACK:
		return "playback";
	case CORD_AUDIO_SOURCE_MIC:
		return "mic";
	case CORD_AUDIO_SOURCE_MIC_VOICE_COMMUNICATION:
		return "mic-voice-communication";
	default:
		return "output";
	}
}

const char *cord_video_codec_name(enum cord_video_codec codec)
{
	switch (codec) {
	case CORD_VIDEO_CODEC_H264:
		return "h264";
	case CORD_VIDEO_CODEC_H265:
		return "h265";
	case CORD_VIDEO_CODEC_AV1:
		return "av1";
	default:
		return "h264";
	}
}

void cord_parse_packet_header(const uint8_t buf[CORD_PACKET_HEADER_SIZE],
			      struct cord_packet_header *out)
{
	const uint64_t flags_pts = cord_read64be(buf);

	out->is_session = (buf[0] & 0x80) != 0;

	if (out->is_session) {
		/* Header-only: resolution announcement on the video stream. */
		out->is_config = false;
		out->is_key_frame = false;
		out->pts_us = 0;
		out->size = 0;
		out->width = cord_read32be(&buf[4]);
		out->height = cord_read32be(&buf[8]);
		return;
	}

	out->is_config = (flags_pts & CORD_PACKET_FLAG_CONFIG) != 0;
	out->is_key_frame = (flags_pts & CORD_PACKET_FLAG_KEY_FRAME) != 0;
	out->pts_us = flags_pts & CORD_PACKET_PTS_MASK;
	out->size = cord_read32be(&buf[8]);
	out->width = 0;
	out->height = 0;
}
