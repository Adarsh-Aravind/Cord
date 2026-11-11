/*
 * Cord - scrcpy device-server wire protocol.
 *
 * ALL constants here are transcribed from the scrcpy v5.0.1 sources:
 *   app/src/server.c   - launch command, socket order, device-info header
 *   app/src/demuxer.c  - codec ids, packet framing
 *   app/src/server.h   - SC_DEVICE_NAME_FIELD_LENGTH
 *
 * The protocol is NOT stable across scrcpy releases. If you bump the bundled
 * scrcpy-server binary, re-read those three files and update this header in
 * lockstep. CORD_SCRCPY_VERSION must match the jar exactly: the server
 * compares it against its own and exits if it differs.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

/* Must equal the version of data/bin/scrcpy-server byte-for-byte. */
#define CORD_SCRCPY_VERSION "5.0.1"

/* Where the jar is pushed, and the abstract socket the server listens on. */
#define CORD_DEVICE_SERVER_PATH "/data/local/tmp/scrcpy-server.jar"
#define CORD_SOCKET_NAME_PREFIX "scrcpy_"
#define CORD_SERVER_CLASS       "com.genymobile.scrcpy.Server"

/* First socket carries a fixed 64-byte, NUL-padded device name. */
#define CORD_DEVICE_NAME_FIELD_LENGTH 64

/* Each stream then opens with a 4-byte big-endian codec id. */
#define CORD_CODEC_ID_H264 UINT32_C(0x68323634) /* "h264" */
#define CORD_CODEC_ID_H265 UINT32_C(0x68323635) /* "h265" */
#define CORD_CODEC_ID_AV1  UINT32_C(0x00617631) /* "av1"  */
#define CORD_CODEC_ID_OPUS UINT32_C(0x6f707573) /* "opus" */
#define CORD_CODEC_ID_AAC  UINT32_C(0x00616163) /* "aac"  */
#define CORD_CODEC_ID_FLAC UINT32_C(0x666c6163) /* "flac" */
#define CORD_CODEC_ID_RAW  UINT32_C(0x00726177) /* "raw" = PCM s16le */

/*
 * Every packet is prefixed with a 12-byte header.
 *
 * MSB set -> "session" packet (video stream only), header only, no payload:
 *   [0]      0x80 | ... , [3] bit0 = client-resized flag
 *   [4..7]   video width   (u32 BE)
 *   [8..11]  video height  (u32 BE)
 *
 * MSB clear -> media packet: 12-byte header then `size` payload bytes:
 *   [0..7]   flags|PTS (u64 BE): bit62 = config, bit61 = key frame,
 *                                bits 0..60 = PTS in microseconds
 *   [8..11]  payload size (u32 BE)
 */
#define CORD_PACKET_HEADER_SIZE 12
#define CORD_PACKET_FLAG_SESSION   (UINT64_C(1) << 63)
#define CORD_PACKET_FLAG_CONFIG    (UINT64_C(1) << 62)
#define CORD_PACKET_FLAG_KEY_FRAME (UINT64_C(1) << 61)
#define CORD_PACKET_PTS_MASK       (CORD_PACKET_FLAG_KEY_FRAME - 1)

/* Device-side audio capture source, passed as audio_source=<name>. */
enum cord_audio_source {
	/*
	 * Full device output mix via REMOTE_SUBMIX. Works because the server
	 * runs as uid=shell, which holds CAPTURE_AUDIO_OUTPUT. This is the one
	 * source that carries in-game teammate voice chat, so it is the default.
	 */
	CORD_AUDIO_SOURCE_OUTPUT = 0,
	/* Per-app MediaProjection capture, Android 13+. Apps may opt out. */
	CORD_AUDIO_SOURCE_PLAYBACK,
	/* The phone's own microphone. */
	CORD_AUDIO_SOURCE_MIC,
	CORD_AUDIO_SOURCE_MIC_VOICE_COMMUNICATION,
};

const char *cord_audio_source_name(enum cord_audio_source src);

enum cord_video_codec {
	CORD_VIDEO_CODEC_H264 = 0,
	CORD_VIDEO_CODEC_H265,
	CORD_VIDEO_CODEC_AV1,
};

const char *cord_video_codec_name(enum cord_video_codec codec);

/* Big-endian readers; the wire format is network byte order throughout. */
static inline uint32_t cord_read32be(const uint8_t *b)
{
	return ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) |
	       ((uint32_t)b[2] << 8) | (uint32_t)b[3];
}

static inline uint64_t cord_read64be(const uint8_t *b)
{
	return ((uint64_t)cord_read32be(b) << 32) | cord_read32be(b + 4);
}

/* Parsed 12-byte packet header. */
struct cord_packet_header {
	bool is_session;   /* header-only resolution announcement */
	bool is_config;    /* SPS/PPS or codec config, no usable PTS */
	bool is_key_frame;
	uint64_t pts_us;   /* valid when !is_session && !is_config */
	uint32_t size;     /* payload bytes following the header */
	uint32_t width;    /* valid when is_session */
	uint32_t height;   /* valid when is_session */
};

void cord_parse_packet_header(const uint8_t buf[CORD_PACKET_HEADER_SIZE],
			      struct cord_packet_header *out);
