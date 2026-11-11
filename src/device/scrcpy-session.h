/*
 * Cord - device session lifecycle.
 *
 * Owns one connection to one phone: pushes the server jar, sets up the adb
 * tunnel, launches the device-side server, performs the handshake, and runs the
 * video and audio reader threads.
 *
 * Latency note: the reader threads invoke the callbacks directly, on the thread
 * that read the bytes off the socket. There is deliberately NO intermediate
 * queue between socket and decoder, and none between decoder and OBS. Every
 * queue in this path is a frame of added latency, and the whole point of the
 * product is that the picture is not behind the phone.
 */
#pragma once

#include "adb-client.h"
#include "scrcpy-proto.h"

#include <stdbool.h>
#include <stdint.h>

struct cord_session_config {
	char serial[CORD_SERIAL_MAX];

	/* video */
	enum cord_video_codec video_codec;
	uint32_t video_bit_rate; /* bits/s; 0 = server default */
	uint32_t max_fps;        /* 0 = unlimited */
	uint32_t max_size;       /* longest edge in px; 0 = native */

	/* audio */
	bool audio_enabled;
	enum cord_audio_source audio_source;
	uint32_t audio_bit_rate; /* bits/s; 0 = server default */
	/*
	 * Keep the audio playing on the phone as well as capturing it.
	 * Android 13+ only, and it forces the per-app "playback" source, which
	 * apps are allowed to opt out of. The default "output" source uses
	 * REMOTE_SUBMIX, which takes the output mix over and silences the
	 * phone - so this is the only way to hear the game on the phone itself.
	 */
	bool audio_dup;

	/* Keep the phone awake while streaming, and restore on disconnect. */
	bool stay_awake;
};

void cord_session_config_defaults(struct cord_session_config *cfg);

/*
 * Callbacks fire from the reader threads. Keep them short and non-blocking.
 * `data` is only valid for the duration of the call.
 */
struct cord_session_callbacks {
	void *ctx;

	/* Called once per stream, before any packet, with the scrcpy codec id. */
	void (*on_video_codec)(void *ctx, uint32_t codec_id);
	void (*on_audio_codec)(void *ctx, uint32_t codec_id);

	/* Video resolution announcement; also fires on rotation. */
	void (*on_video_size)(void *ctx, uint32_t width, uint32_t height);

	void (*on_video_packet)(void *ctx,
				const struct cord_packet_header *hdr,
				const uint8_t *data);
	void (*on_audio_packet)(void *ctx,
				const struct cord_packet_header *hdr,
				const uint8_t *data);

	/*
	 * The device went away (unplugged, server died, screen locked on some
	 * ROMs). Fires at most once per session. Do not call cord_session_stop()
	 * from inside it; set a flag and let the owner tear down.
	 */
	void (*on_disconnected)(void *ctx);
};

struct cord_session;

/*
 * Starts a session. Blocks for the handshake (typically 300-800ms, dominated by
 * the jar push on first run), then returns with the reader threads running.
 * Returns NULL on failure, having logged why and cleaned up every tunnel it
 * created.
 */
struct cord_session *cord_session_start(const struct cord_session_config *cfg,
					const struct cord_session_callbacks *cb);

/* Joins the reader threads and tears down the device process and tunnels. */
void cord_session_stop(struct cord_session *session);

/* The device's own name, as reported in the 64-byte handshake field. */
const char *cord_session_device_name(const struct cord_session *session);

/* False once a reader thread has hit EOF. */
bool cord_session_is_alive(const struct cord_session *session);
