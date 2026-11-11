#include "scrcpy-session.h"

#include <obs-module.h>
#include <util/platform.h>
#include <util/threading.h>
#include <util/dstr.h>

#include <stdlib.h>
#include <string.h>

#define LOG(level, fmt, ...) \
	blog(level, "[cord/session] " fmt, ##__VA_ARGS__)

/* How long to wait for the device to call back through the reverse tunnel. */
#define CORD_CONNECT_TIMEOUT_MS 10000

/* Largest packet we will accept, as a guard against a desynced stream. */
#define CORD_MAX_PACKET_SIZE (16 * 1024 * 1024)

struct cord_session {
	struct cord_session_config cfg;
	struct cord_session_callbacks cb;

	char socket_name[32]; /* scrcpy_xxxxxxxx */
	char device_name[CORD_DEVICE_NAME_FIELD_LENGTH];

	/* Streams the device server's stdout/stderr; closing it kills it. */
	cord_socket_t process_sock;
	cord_socket_t video_sock;
	cord_socket_t audio_sock;

	/* Tunnel bookkeeping, so teardown undoes exactly what we created. */
	bool tunnel_is_forward;
	uint16_t tunnel_port;

	pthread_t video_thread;
	pthread_t audio_thread;
	pthread_t log_thread;
	bool video_thread_started;
	bool audio_thread_started;
	bool log_thread_started;

	volatile bool stopping;
	volatile bool alive;
	volatile long disconnect_reported;
};

void cord_session_config_defaults(struct cord_session_config *cfg)
{
	memset(cfg, 0, sizeof(*cfg));
	cfg->video_codec = CORD_VIDEO_CODEC_H264;
	cfg->video_bit_rate = 8000000;
	cfg->max_fps = 60;
	cfg->max_size = 0;
	cfg->audio_enabled = true;
	/* The full output mix: the only source that carries teammate voice. */
	cfg->audio_source = CORD_AUDIO_SOURCE_OUTPUT;
	/*
	 * Opus, stereo. The server defaults to 128k; over USB the bandwidth is
	 * free next to an 8 Mbit video stream, and game audio is wideband
	 * music-like content where 128k is audibly short of transparent.
	 */
	cfg->audio_bit_rate = 192000;
	cfg->stay_awake = true;
}

static void report_disconnect(struct cord_session *s)
{
	if (s->stopping)
		return;
	/*
	 * Fire at most once, whichever reader thread notices first.
	 *
	 * compare_SWAP, not compare_EXCHANGE: the exchange variant takes the
	 * expected value by pointer and writes the previous value back through
	 * it. This one takes plain values, which is what is wanted here.
	 */
	if (os_atomic_compare_swap_long(&s->disconnect_reported, 0, 1)) {
		s->alive = false;
		if (s->cb.on_disconnected)
			s->cb.on_disconnected(s->cb.ctx);
	}
}

/* ------------------------------------------------------------------ */
/* reader threads                                                      */
/* ------------------------------------------------------------------ */

/*
 * Shared loop for both streams. The framing is identical; only the callbacks
 * and the session-packet handling differ (session packets are video-only).
 */
static void stream_loop(struct cord_session *s, cord_socket_t sock,
			bool is_video)
{
	uint8_t header[CORD_PACKET_HEADER_SIZE];
	uint8_t *payload = NULL;
	size_t payload_cap = 0;

	for (;;) {
		if (s->stopping)
			break;

		if (!cord_socket_recv_all(sock, header, sizeof(header))) {
			if (!s->stopping)
				LOG(LOG_INFO, "%s stream ended",
				    is_video ? "video" : "audio");
			break;
		}

		struct cord_packet_header hdr;
		cord_parse_packet_header(header, &hdr);

		if (hdr.is_session) {
			/* Header-only: a new resolution, including on rotate. */
			if (is_video && s->cb.on_video_size)
				s->cb.on_video_size(s->cb.ctx, hdr.width,
						    hdr.height);
			continue;
		}

		if (hdr.size == 0 || hdr.size > CORD_MAX_PACKET_SIZE) {
			LOG(LOG_ERROR,
			    "%s stream desynced (packet size %u), dropping session",
			    is_video ? "video" : "audio", hdr.size);
			break;
		}

		if (hdr.size > payload_cap) {
			payload = brealloc(payload, hdr.size);
			payload_cap = hdr.size;
		}

		if (!cord_socket_recv_all(sock, payload, hdr.size)) {
			if (!s->stopping)
				LOG(LOG_INFO, "%s stream truncated",
				    is_video ? "video" : "audio");
			break;
		}

		/*
		 * Straight into the decoder on this thread: no queue, no hop.
		 */
		if (is_video) {
			if (s->cb.on_video_packet)
				s->cb.on_video_packet(s->cb.ctx, &hdr, payload);
		} else {
			if (s->cb.on_audio_packet)
				s->cb.on_audio_packet(s->cb.ctx, &hdr, payload);
		}
	}

	bfree(payload);
	report_disconnect(s);
}

static void *video_thread_fn(void *arg)
{
	struct cord_session *s = arg;
	os_set_thread_name("cord: video");
	stream_loop(s, s->video_sock, true);
	return NULL;
}

static void *audio_thread_fn(void *arg)
{
	struct cord_session *s = arg;
	os_set_thread_name("cord: audio");
	stream_loop(s, s->audio_sock, false);
	return NULL;
}

/*
 * Relays the device server's stdout/stderr into the OBS log. Without this the
 * most common failures (encoder refused the resolution, audio capture denied,
 * version mismatch) are completely silent.
 */
static void *log_thread_fn(void *arg)
{
	struct cord_session *s = arg;
	os_set_thread_name("cord: device log");

	struct dstr line = {0};
	char chunk[512];

	for (;;) {
		int r = recv(s->process_sock, chunk, sizeof(chunk) - 1, 0);
		if (r <= 0)
			break;
		chunk[r] = 0;

		for (int i = 0; i < r; i++) {
			if (chunk[i] == '\n' || chunk[i] == '\r') {
				if (line.len) {
					LOG(LOG_INFO, "device: %s", line.array);
					dstr_free(&line);
				}
			} else {
				dstr_ncat(&line, &chunk[i], 1);
			}
		}
	}

	if (line.len)
		LOG(LOG_INFO, "device: %s", line.array);
	dstr_free(&line);

	/* The server process exiting is itself a disconnect. */
	report_disconnect(s);
	return NULL;
}

/* ------------------------------------------------------------------ */
/* startup                                                             */
/* ------------------------------------------------------------------ */

/*
 * The scid names the abstract socket, so two sessions (or a stale server from a
 * previous crash) never collide. Java parses it as a signed int, so keep it
 * below 0x80000000.
 */
static uint32_t generate_scid(void)
{
	/*
	 * Only needs to be collision-free, not unguessable: it exists so two
	 * sessions, or a stale server from a previous crash, cannot share a
	 * socket name. Clock plus stack address is plenty.
	 */
	uint32_t local;
	uint64_t ns = os_gettime_ns();
	uint32_t scid = (uint32_t)(ns ^ (ns >> 32)) ^
			(uint32_t)(uintptr_t)&local;
	return scid & 0x7fffffff;
}

/*
 * Forward mode: the server listens on the device and we dial in through the
 * tunnel. adb accepts our connection even when nothing is listening on the far
 * end, so the server sends one dummy byte to prove the far end is really there.
 * That byte must be consumed here or every subsequent read is off by one.
 */
static cord_socket_t forward_connect(uint16_t port, int attempts, int delay_ms)
{
	while (attempts-- > 0) {
		cord_socket_t sock = cord_socket_connect_localhost(port);
		if (sock != CORD_INVALID_SOCKET) {
			cord_socket_set_recv_timeout(sock, 2000);

			char dummy;
			if (cord_socket_recv_all(sock, &dummy, 1)) {
				cord_socket_set_recv_timeout(sock, 0);
				return sock;
			}
			/* Tunnel is up but the server is not listening yet. */
			cord_socket_close(sock);
		}
		os_sleep_ms(delay_ms);
	}
	return CORD_INVALID_SOCKET;
}

/* Builds the `shell:` service string that launches the device server. */
static void build_server_command(const struct cord_session_config *cfg,
				 uint32_t scid, bool tunnel_forward,
				 struct dstr *out)
{
	dstr_printf(out,
		    "shell:CLASSPATH=%s app_process / %s %s"
		    " scid=%08x log_level=info",
		    CORD_DEVICE_SERVER_PATH, CORD_SERVER_CLASS,
		    CORD_SCRCPY_VERSION, scid);

	/*
	 * We never send input to the phone - the player touches the phone
	 * itself - so the control channel is off. That removes a socket and
	 * sidesteps the clipboard sync that can stall the device server.
	 */
	dstr_cat(out, " control=false clipboard_autosync=false");

	if (tunnel_forward)
		dstr_cat(out, " tunnel_forward=true");

	if (cfg->video_codec != CORD_VIDEO_CODEC_H264)
		dstr_catf(out, " video_codec=%s",
			  cord_video_codec_name(cfg->video_codec));

	if (cfg->video_bit_rate)
		dstr_catf(out, " video_bit_rate=%u",
			  (unsigned)cfg->video_bit_rate);

	if (cfg->max_fps)
		dstr_catf(out, " max_fps=%u", (unsigned)cfg->max_fps);

	if (cfg->max_size)
		dstr_catf(out, " max_size=%u", (unsigned)cfg->max_size);

	if (!cfg->audio_enabled) {
		dstr_cat(out, " audio=false");
	} else if (cfg->audio_dup) {
		/*
		 * Duplication implies the playback source; the server rejects
		 * audio_dup with anything else.
		 */
		dstr_cat(out, " audio_source=playback audio_dup=true");
		if (cfg->audio_bit_rate)
			dstr_catf(out, " audio_bit_rate=%u",
				  (unsigned)cfg->audio_bit_rate);
	} else {
		/* "output" is the server default, so only pass the others. */
		if (cfg->audio_source != CORD_AUDIO_SOURCE_OUTPUT)
			dstr_catf(out, " audio_source=%s",
				  cord_audio_source_name(cfg->audio_source));
		if (cfg->audio_bit_rate)
			dstr_catf(out, " audio_bit_rate=%u",
				  (unsigned)cfg->audio_bit_rate);
	}

	if (cfg->stay_awake)
		dstr_cat(out, " stay_awake=true");

	/* Let the server restore device state (awake, power) when we detach. */
	dstr_cat(out, " cleanup=true");
}

/* Reads the 64-byte device-name field that precedes everything else. */
static bool read_device_meta(struct cord_session *s, cord_socket_t first)
{
	uint8_t buf[CORD_DEVICE_NAME_FIELD_LENGTH];
	if (!cord_socket_recv_all(first, buf, sizeof(buf))) {
		LOG(LOG_ERROR, "could not read device info");
		return false;
	}
	buf[sizeof(buf) - 1] = 0; /* in case the device sends garbage */
	memcpy(s->device_name, buf, sizeof(buf));
	return true;
}

/*
 * Reads and validates the 4-byte big-endian codec id that opens a stream.
 *
 * The device uses two sentinel values in place of a codec id, and both are
 * ordinary outcomes rather than bugs:
 *   0 - the device disabled this stream. For audio this is the common case on
 *       Android 11 and on ROMs that refuse output capture, and it is exactly
 *       the "teammate voice is silent" symptom, so it gets an explicit message.
 *   1 - the device could not configure the stream (no encoder for the requested
 *       codec, or a resolution the encoder rejected).
 */
static bool read_codec_id(cord_socket_t sock, uint32_t *out, const char *what)
{
	uint8_t buf[4];
	if (!cord_socket_recv_all(sock, buf, 4)) {
		LOG(LOG_ERROR, "could not read the %s codec id", what);
		return false;
	}

	*out = cord_read32be(buf);

	if (*out == 0) {
		LOG(LOG_ERROR, "the device disabled its %s stream", what);
		return false;
	}

	if (*out == 1) {
		LOG(LOG_ERROR,
		    "the device could not configure its %s stream; try a "
		    "different codec or a lower resolution",
		    what);
		return false;
	}

	return true;
}

static void teardown_tunnel(struct cord_session *s)
{
	if (s->tunnel_is_forward) {
		if (s->tunnel_port)
			cord_adb_forward_remove(s->cfg.serial, s->tunnel_port);
	} else {
		cord_adb_reverse_remove(s->cfg.serial, s->socket_name);
	}
	s->tunnel_port = 0;
}

struct cord_session *cord_session_start(const struct cord_session_config *cfg,
					const struct cord_session_callbacks *cb)
{
	if (!cfg->serial[0]) {
		LOG(LOG_ERROR, "no device selected");
		return NULL;
	}

	struct cord_session *s = bzalloc(sizeof(struct cord_session));
	s->cfg = *cfg;
	s->cb = *cb;
	s->process_sock = CORD_INVALID_SOCKET;
	s->video_sock = CORD_INVALID_SOCKET;
	s->audio_sock = CORD_INVALID_SOCKET;

	const uint32_t scid = generate_scid();
	snprintf(s->socket_name, sizeof(s->socket_name), "%s%08x",
		 CORD_SOCKET_NAME_PREFIX, scid);

	/* --- sanity-check the device before doing any work on it --- */
	int sdk = cord_adb_sdk_version(cfg->serial);
	if (sdk && sdk < 30) {
		LOG(LOG_ERROR,
		    "device reports Android API %d; audio capture needs API 30 "
		    "(Android 11) or newer",
		    sdk);
		if (cfg->audio_enabled) {
			LOG(LOG_WARNING,
			    "continuing with audio disabled for this device");
			s->cfg.audio_enabled = false;
		}
	}

	/* --- push the server jar --- */
	char *jar = obs_module_file("bin/scrcpy-server");
	if (!jar || !os_file_exists(jar)) {
		LOG(LOG_ERROR,
		    "bundled scrcpy-server is missing; run scripts/fetch-deps.ps1");
		bfree(jar);
		goto fail;
	}

	if (!cord_adb_push(cfg->serial, jar, CORD_DEVICE_SERVER_PATH)) {
		bfree(jar);
		goto fail;
	}
	bfree(jar);

	/* --- tunnel: reverse preferred, forward as fallback --- */
	cord_socket_t listener = CORD_INVALID_SOCKET;
	uint16_t port = 0;

	listener = cord_socket_listen_local(&port);
	if (listener == CORD_INVALID_SOCKET) {
		LOG(LOG_ERROR, "could not open a local listening socket");
		goto fail;
	}

	s->tunnel_port = port;
	s->tunnel_is_forward = false;

	if (!cord_adb_reverse(cfg->serial, s->socket_name, port)) {
		/*
		 * Some OEM ROMs and old devices refuse reverse tunnels. Forward
		 * mode works everywhere but needs the server to listen instead,
		 * and sends a dummy byte we have to consume.
		 */
		LOG(LOG_WARNING,
		    "reverse tunnel refused, falling back to forward mode");
		cord_socket_close(listener);
		listener = CORD_INVALID_SOCKET;

		s->tunnel_is_forward = true;
		if (!cord_adb_forward(cfg->serial, port, s->socket_name)) {
			LOG(LOG_ERROR, "forward tunnel also failed");
			goto fail;
		}
	}

	/* --- launch the device server --- */
	struct dstr command = {0};
	build_server_command(&s->cfg, scid, s->tunnel_is_forward, &command);
	LOG(LOG_INFO, "launching: %s", command.array);

	s->process_sock = cord_adb_open_service(cfg->serial, command.array);
	dstr_free(&command);

	if (s->process_sock == CORD_INVALID_SOCKET) {
		LOG(LOG_ERROR, "could not start the device server");
		if (listener != CORD_INVALID_SOCKET)
			cord_socket_close(listener);
		goto fail;
	}

	/* --- accept/connect the streams, in the server's own order --- */
	if (!s->tunnel_is_forward) {
		s->video_sock = cord_socket_accept_timeout(
			listener, CORD_CONNECT_TIMEOUT_MS);
		if (s->video_sock == CORD_INVALID_SOCKET) {
			LOG(LOG_ERROR,
			    "device never connected back; if the screen is "
			    "locked, unlock it and try again");
			cord_socket_close(listener);
			goto fail;
		}

		if (s->cfg.audio_enabled) {
			s->audio_sock = cord_socket_accept_timeout(
				listener, CORD_CONNECT_TIMEOUT_MS);
			if (s->audio_sock == CORD_INVALID_SOCKET) {
				LOG(LOG_WARNING,
				    "audio stream never connected; continuing "
				    "without phone audio");
				s->cfg.audio_enabled = false;
			}
		}
		cord_socket_close(listener);
	} else {
		/* 100 attempts x 100ms: the server needs time to come up. */
		s->video_sock = forward_connect(port, 100, 100);
		if (s->video_sock == CORD_INVALID_SOCKET) {
			LOG(LOG_ERROR,
			    "could not reach the device server through the "
			    "forward tunnel");
			goto fail;
		}

		if (s->cfg.audio_enabled) {
			/* Already proven reachable, so a short retry is enough. */
			s->audio_sock = forward_connect(port, 10, 100);
			if (s->audio_sock == CORD_INVALID_SOCKET) {
				LOG(LOG_WARNING,
				    "audio stream did not open; continuing "
				    "without phone audio");
				s->cfg.audio_enabled = false;
			}
		}
	}

	/* The tunnel has done its job; remove it so nothing leaks. */
	teardown_tunnel(s);

	/* --- handshake --- */
	if (!read_device_meta(s, s->video_sock))
		goto fail;

	LOG(LOG_INFO, "connected to '%s'", s->device_name);

	uint32_t video_codec_id = 0;
	if (!read_codec_id(s->video_sock, &video_codec_id, "video"))
		goto fail;
	if (s->cb.on_video_codec)
		s->cb.on_video_codec(s->cb.ctx, video_codec_id);

	if (s->cfg.audio_enabled) {
		uint32_t audio_codec_id = 0;
		if (!read_codec_id(s->audio_sock, &audio_codec_id, "audio")) {
			/*
			 * Video is still perfectly usable, so never fail the
			 * whole session over audio - but say what to try, since
			 * this is the symptom users notice first.
			 */
			LOG(LOG_WARNING,
			    "no phone audio for this session. Things to try: "
			    "unlock the screen and reconnect (Android 11 needs "
			    "the device unlocked when capture starts); make "
			    "sure something is actually playing; or switch the "
			    "audio source to 'Phone microphone' if this ROM "
			    "blocks output capture.");
			s->cfg.audio_enabled = false;
		} else if (s->cb.on_audio_codec) {
			s->cb.on_audio_codec(s->cb.ctx, audio_codec_id);
		}
	}

	/*
	 * Nagle would coalesce small packets and add tens of milliseconds on
	 * the audio stream in particular. Off on both.
	 */
	cord_socket_set_nodelay(s->video_sock, true);
	if (s->cfg.audio_enabled)
		cord_socket_set_nodelay(s->audio_sock, true);

	/* --- reader threads --- */
	s->alive = true;

	if (pthread_create(&s->log_thread, NULL, log_thread_fn, s) == 0)
		s->log_thread_started = true;

	if (pthread_create(&s->video_thread, NULL, video_thread_fn, s) != 0) {
		LOG(LOG_ERROR, "could not start the video thread");
		goto fail;
	}
	s->video_thread_started = true;

	if (s->cfg.audio_enabled) {
		if (pthread_create(&s->audio_thread, NULL, audio_thread_fn,
				   s) != 0) {
			LOG(LOG_WARNING,
			    "could not start the audio thread; video only");
			s->cfg.audio_enabled = false;
		} else {
			s->audio_thread_started = true;
		}
	}

	return s;

fail:
	cord_session_stop(s);
	return NULL;
}

void cord_session_stop(struct cord_session *s)
{
	if (!s)
		return;

	s->stopping = true;
	s->alive = false;

	/*
	 * Shut the sockets down before joining: a reader thread parked in
	 * recv() only wakes when its socket is closed, and closing the process
	 * socket is also what terminates app_process on the device.
	 */
	if (s->video_sock != CORD_INVALID_SOCKET) {
		shutdown(s->video_sock, CORD_SHUT_RDWR);
		cord_socket_close(s->video_sock);
		s->video_sock = CORD_INVALID_SOCKET;
	}
	if (s->audio_sock != CORD_INVALID_SOCKET) {
		shutdown(s->audio_sock, CORD_SHUT_RDWR);
		cord_socket_close(s->audio_sock);
		s->audio_sock = CORD_INVALID_SOCKET;
	}
	if (s->process_sock != CORD_INVALID_SOCKET) {
		shutdown(s->process_sock, CORD_SHUT_RDWR);
		cord_socket_close(s->process_sock);
		s->process_sock = CORD_INVALID_SOCKET;
	}

	if (s->video_thread_started)
		pthread_join(s->video_thread, NULL);
	if (s->audio_thread_started)
		pthread_join(s->audio_thread, NULL);
	if (s->log_thread_started)
		pthread_join(s->log_thread, NULL);

	/* Harmless if already removed after a successful handshake. */
	teardown_tunnel(s);

	bfree(s);
}

const char *cord_session_device_name(const struct cord_session *s)
{
	return s ? s->device_name : "";
}

bool cord_session_is_alive(const struct cord_session *s)
{
	return s && s->alive;
}
