/*
 * Cord - the "Phone (Cord)" OBS source.
 *
 * Owns a device session and the two decoders, and presents everything to OBS as
 * one source carrying both video and audio. Audio lands on whichever tracks the
 * user picks in Advanced Audio Properties, so the phone can be split out from
 * the PC microphone in a multi-track recording.
 *
 * Threading:
 *   - a supervisor thread owns session start/stop, so creating the source never
 *     blocks the OBS UI for the ~500ms handshake, and so a disconnect can
 *     reconnect by itself.
 *   - the session's reader threads call straight into the decoders, which call
 *     straight into libobs. `lock` guards only decoder creation and teardown
 *     against those readers.
 */

#include "../audio/audio-decode.h"
#include "../audio/wasapi-player.h"
#include "../device/adb-client.h"
#include "../device/device-cache.h"
#include "../device/scrcpy-session.h"
#include "../micsend/micsend-control.h"
#include "../util/cord-clock.h"
#include "../video/decoder.h"

#include <obs-module.h>
#include <util/dstr.h>
#include <util/platform.h>

#include <math.h>
#include <util/threading.h>

#include <string.h>

#define LOG(level, fmt, ...) \
	blog(level, "[cord/source] " fmt, ##__VA_ARGS__)

#define S_DEVICE "device"
#define S_MAX_SIZE "max_size"
#define S_BITRATE "video_bit_rate"
#define S_MAX_FPS "max_fps"
#define S_CODEC "video_codec"
#define S_AUDIO_ENABLED "audio_enabled"
#define S_AUDIO_SOURCE "audio_source"
#define S_STAY_AWAKE "stay_awake"
#define S_AUTO_RECONNECT "auto_reconnect"
#define S_AUDIO_DUP "audio_dup"
#define S_MONITOR_ENABLED "monitor_enabled"
#define S_MONITOR_DEVICE "monitor_device"
#define S_MONITOR_VOLUME "monitor_volume_db"

/* How long to wait before retrying a failed or dropped connection. */
#define CORD_RETRY_DELAY_MS 2000

struct cord_phone_source {
	obs_source_t *source;

	pthread_mutex_t lock;
	struct cord_session *session;
	struct cord_decoder *video_dec;
	struct cord_audio_decoder *audio_dec;
	struct cord_clock clock;

	/* Config, copied under `lock` whenever settings change. */
	struct cord_session_config cfg;
	bool auto_reconnect;
	bool settings_dirty;

	/* Last announced frame size, for get_width/get_height. */
	volatile uint32_t width;
	volatile uint32_t height;

	/*
	 * Local monitor: plays the phone's audio on a PC output directly,
	 * bypassing OBS monitoring. Needed because the default capture source
	 * silences the phone itself. See audio/wasapi-player.h.
	 */
	struct cord_audio_player *player;
	bool monitor_enabled;
	char monitor_device[512];
	float monitor_volume;

	/* PC mic -> phone analog send; see micsend-control.h. */
	struct cord_micsend_control micsend;
	bool micsend_ready;

	pthread_t supervisor;
	bool supervisor_started;
	os_event_t *stop_signal;
	volatile bool shutting_down;
	volatile bool session_lost;
};

/* ------------------------------------------------------------------ */
/* session callbacks (called from the session's reader threads)         */
/* ------------------------------------------------------------------ */

static void on_video_codec(void *data, uint32_t codec_id)
{
	struct cord_phone_source *ctx = data;

	pthread_mutex_lock(&ctx->lock);
	cord_decoder_destroy(ctx->video_dec);
	ctx->video_dec =
		cord_decoder_create(codec_id, ctx->source, &ctx->clock);
	pthread_mutex_unlock(&ctx->lock);
}

static void on_audio_codec(void *data, uint32_t codec_id)
{
	struct cord_phone_source *ctx = data;

	pthread_mutex_lock(&ctx->lock);
	cord_audio_decoder_destroy(ctx->audio_dec);
	ctx->audio_dec = cord_audio_decoder_create(codec_id, ctx->source,
						   &ctx->clock);
	/* A new session means a new decoder, so re-attach the monitor. */
	cord_audio_decoder_set_player(ctx->audio_dec, ctx->player);
	pthread_mutex_unlock(&ctx->lock);
}

static void on_video_size(void *data, uint32_t width, uint32_t height)
{
	struct cord_phone_source *ctx = data;

	if (width && height && (ctx->width != width || ctx->height != height)) {
		LOG(LOG_INFO, "device video size is now %ux%u", width, height);
		ctx->width = width;
		ctx->height = height;
	}
}

static void on_video_packet(void *data, const struct cord_packet_header *hdr,
			    const uint8_t *payload)
{
	struct cord_phone_source *ctx = data;

	pthread_mutex_lock(&ctx->lock);
	if (ctx->video_dec)
		cord_decoder_feed(ctx->video_dec, hdr, payload);
	pthread_mutex_unlock(&ctx->lock);
}

static void on_audio_packet(void *data, const struct cord_packet_header *hdr,
			    const uint8_t *payload)
{
	struct cord_phone_source *ctx = data;

	pthread_mutex_lock(&ctx->lock);
	if (ctx->audio_dec)
		cord_audio_decoder_feed(ctx->audio_dec, hdr, payload);
	pthread_mutex_unlock(&ctx->lock);
}

static void on_disconnected(void *data)
{
	struct cord_phone_source *ctx = data;

	/*
	 * Only flag it. Tearing the session down here would mean a reader
	 * thread joining itself, and the supervisor is already watching.
	 */
	ctx->session_lost = true;
}

/* ------------------------------------------------------------------ */
/* supervisor                                                          */
/* ------------------------------------------------------------------ */

static void teardown_session(struct cord_phone_source *ctx)
{
	/*
	 * Stop the session first, outside the lock: it joins reader threads,
	 * and those threads take the lock to reach the decoders. Holding it
	 * here would deadlock.
	 */
	struct cord_session *session = ctx->session;
	ctx->session = NULL;

	if (session)
		cord_session_stop(session);

	pthread_mutex_lock(&ctx->lock);
	cord_decoder_destroy(ctx->video_dec);
	ctx->video_dec = NULL;
	cord_audio_decoder_destroy(ctx->audio_dec);
	ctx->audio_dec = NULL;
	pthread_mutex_unlock(&ctx->lock);

	/*
	 * The next session starts a fresh device clock, so the anchor must go
	 * too - otherwise its first PTS is interpreted against the old one and
	 * every frame looks hours late.
	 */
	cord_clock_reset(&ctx->clock);
	ctx->session_lost = false;
}

static bool wait_ms(struct cord_phone_source *ctx, int ms)
{
	/* Returns false when asked to shut down. */
	return os_event_timedwait(ctx->stop_signal, ms) == ETIMEDOUT;
}

static void *supervisor_thread(void *arg)
{
	struct cord_phone_source *ctx = arg;
	os_set_thread_name("cord: supervisor");

	while (!ctx->shutting_down) {
		/* A settings change restarts the session with the new config. */
		if (ctx->session && ctx->settings_dirty) {
			LOG(LOG_INFO, "settings changed, reconnecting");
			ctx->settings_dirty = false;
			teardown_session(ctx);
		}

		if (ctx->session && ctx->session_lost) {
			LOG(LOG_WARNING, "device disconnected");
			teardown_session(ctx);

			if (!ctx->auto_reconnect) {
				LOG(LOG_INFO,
				    "auto-reconnect is off; select the device "
				    "again to resume");
				/* Wait to be told to shut down or reconfigured. */
				while (!ctx->shutting_down &&
				       !ctx->settings_dirty) {
					if (!wait_ms(ctx, 250))
						break;
				}
				continue;
			}
		}

		if (!ctx->session) {
			pthread_mutex_lock(&ctx->lock);
			struct cord_session_config cfg = ctx->cfg;
			ctx->settings_dirty = false;
			pthread_mutex_unlock(&ctx->lock);

			if (cfg.serial[0]) {
				struct cord_session_callbacks cb = {
					.ctx = ctx,
					.on_video_codec = on_video_codec,
					.on_audio_codec = on_audio_codec,
					.on_video_size = on_video_size,
					.on_video_packet = on_video_packet,
					.on_audio_packet = on_audio_packet,
					.on_disconnected = on_disconnected,
				};

				ctx->session = cord_session_start(&cfg, &cb);

				if (!ctx->session) {
					LOG(LOG_WARNING,
					    "connection failed, retrying in %d ms",
					    CORD_RETRY_DELAY_MS);
					if (!wait_ms(ctx, CORD_RETRY_DELAY_MS))
						break;
					continue;
				}

				LOG(LOG_INFO, "streaming from %s",
				    cord_session_device_name(ctx->session));
			}
		}

		if (!wait_ms(ctx, 250))
			break;
	}

	teardown_session(ctx);
	return NULL;
}

/* ------------------------------------------------------------------ */
/* obs_source_info                                                     */
/* ------------------------------------------------------------------ */

static const char *source_get_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return obs_module_text("Cord.Source");
}

static void read_settings(struct cord_phone_source *ctx, obs_data_t *settings)
{
	struct cord_session_config cfg;
	cord_session_config_defaults(&cfg);

	const char *serial = obs_data_get_string(settings, S_DEVICE);
	snprintf(cfg.serial, sizeof(cfg.serial), "%s", serial ? serial : "");

	cfg.max_size = (uint32_t)obs_data_get_int(settings, S_MAX_SIZE);
	cfg.video_bit_rate = (uint32_t)obs_data_get_int(settings, S_BITRATE);
	cfg.max_fps = (uint32_t)obs_data_get_int(settings, S_MAX_FPS);
	cfg.video_codec =
		(enum cord_video_codec)obs_data_get_int(settings, S_CODEC);
	cfg.audio_enabled = obs_data_get_bool(settings, S_AUDIO_ENABLED);
	cfg.audio_source = (enum cord_audio_source)obs_data_get_int(
		settings, S_AUDIO_SOURCE);
	cfg.audio_dup = obs_data_get_bool(settings, S_AUDIO_DUP);
	cfg.stay_awake = obs_data_get_bool(settings, S_STAY_AWAKE);

	pthread_mutex_lock(&ctx->lock);
	const bool changed = memcmp(&ctx->cfg, &cfg, sizeof(cfg)) != 0;
	ctx->cfg = cfg;
	ctx->auto_reconnect = obs_data_get_bool(settings, S_AUTO_RECONNECT);
	if (changed)
		ctx->settings_dirty = true;
	pthread_mutex_unlock(&ctx->lock);
}

/*
 * Starts, stops or retunes the local monitor. Called under no lock; takes
 * ctx->lock itself because the audio reader thread reads ctx->player.
 */
static void apply_monitor_settings(struct cord_phone_source *ctx,
				   obs_data_t *settings)
{
	const bool enabled = obs_data_get_bool(settings, S_MONITOR_ENABLED);
	const char *device = obs_data_get_string(settings, S_MONITOR_DEVICE);
	const double volume_db = obs_data_get_double(settings, S_MONITOR_VOLUME);

	const float volume = volume_db <= -30.0
				     ? 0.0f
				     : (float)pow(10.0, volume_db / 20.0);

	/* Only the device needs a rebuild; level is applied live. */
	const bool changed =
		strcmp(ctx->monitor_device, device ? device : "") != 0;

	pthread_mutex_lock(&ctx->lock);

	snprintf(ctx->monitor_device, sizeof(ctx->monitor_device), "%s",
		 device ? device : "");
	ctx->monitor_volume = volume;
	ctx->monitor_enabled = enabled;

	/* Only rebuild when it actually has to be rebuilt. */
	if (ctx->player && (!enabled || changed)) {
		cord_audio_decoder_set_player(ctx->audio_dec, NULL);
		struct cord_audio_player *old = ctx->player;
		ctx->player = NULL;
		pthread_mutex_unlock(&ctx->lock);
		cord_audio_player_destroy(old);
		pthread_mutex_lock(&ctx->lock);
	}

	if (enabled && !ctx->player) {
		ctx->player = cord_audio_player_create(
			ctx->monitor_device, CORD_AUDIO_SAMPLE_RATE,
			CORD_AUDIO_CHANNELS);
		cord_audio_decoder_set_player(ctx->audio_dec, ctx->player);
	}

	if (ctx->player)
		cord_audio_player_set_volume(ctx->player, volume);

	pthread_mutex_unlock(&ctx->lock);
}

static void source_update(void *data, obs_data_t *settings)
{
	struct cord_phone_source *ctx = data;

	read_settings(ctx, settings);
	apply_monitor_settings(ctx, settings);

	if (ctx->micsend_ready)
		cord_micsend_control_update(&ctx->micsend, settings);
}

static void source_destroy(void *data)
{
	struct cord_phone_source *ctx = data;
	if (!ctx)
		return;

	ctx->shutting_down = true;
	if (ctx->stop_signal)
		os_event_signal(ctx->stop_signal);

	if (ctx->supervisor_started)
		pthread_join(ctx->supervisor, NULL);

	teardown_session(ctx);

	if (ctx->player) {
		cord_audio_player_destroy(ctx->player);
		ctx->player = NULL;
	}

	if (ctx->micsend_ready) {
		cord_micsend_control_free(&ctx->micsend);
		ctx->micsend_ready = false;
	}

	if (ctx->stop_signal)
		os_event_destroy(ctx->stop_signal);
	cord_clock_free(&ctx->clock);
	pthread_mutex_destroy(&ctx->lock);
	bfree(ctx);
}

static void *source_create(obs_data_t *settings, obs_source_t *source)
{
	struct cord_phone_source *ctx =
		bzalloc(sizeof(struct cord_phone_source));
	ctx->source = source;

	pthread_mutex_init(&ctx->lock, NULL);
	cord_clock_init(&ctx->clock);

	if (os_event_init(&ctx->stop_signal, OS_EVENT_TYPE_MANUAL) != 0) {
		LOG(LOG_ERROR, "could not create the stop signal");
		source_destroy(ctx);
		return NULL;
	}

	/*
	 * THE latency switch on the OBS side. By default libobs buffers async
	 * frames to smooth out jittery sources, which costs multiple frames of
	 * delay. The phone is a steady 60fps over USB and does not need it, and
	 * the whole point is that the mirror is not behind the phone.
	 */
	obs_source_set_async_unbuffered(source, true);

	/* Warm the device list now, so opening properties is instant. */
	cord_device_cache_ensure_running();

	read_settings(ctx, settings);

	apply_monitor_settings(ctx, settings);

	cord_micsend_control_init(&ctx->micsend, source);
	ctx->micsend_ready = true;
	cord_micsend_control_update(&ctx->micsend, settings);

	if (pthread_create(&ctx->supervisor, NULL, supervisor_thread, ctx) !=
	    0) {
		LOG(LOG_ERROR, "could not start the supervisor thread");
		source_destroy(ctx);
		return NULL;
	}
	ctx->supervisor_started = true;

	return ctx;
}

static uint32_t source_get_width(void *data)
{
	struct cord_phone_source *ctx = data;
	return ctx->width;
}

static uint32_t source_get_height(void *data)
{
	struct cord_phone_source *ctx = data;
	return ctx->height;
}

static void source_defaults(obs_data_t *settings)
{
	obs_data_set_default_int(settings, S_MAX_SIZE, 0); /* native */
	obs_data_set_default_int(settings, S_BITRATE, 8000000);
	obs_data_set_default_int(settings, S_MAX_FPS, 60);
	obs_data_set_default_int(settings, S_CODEC, CORD_VIDEO_CODEC_H264);
	obs_data_set_default_bool(settings, S_AUDIO_ENABLED, true);
	obs_data_set_default_int(settings, S_AUDIO_SOURCE,
				 CORD_AUDIO_SOURCE_OUTPUT);
	obs_data_set_default_bool(settings, S_STAY_AWAKE, true);
	obs_data_set_default_bool(settings, S_AUTO_RECONNECT, true);
	obs_data_set_default_bool(settings, S_AUDIO_DUP, false);
	obs_data_set_default_bool(settings, S_MONITOR_ENABLED, false);
	obs_data_set_default_string(settings, S_MONITOR_DEVICE, "");
	obs_data_set_default_double(settings, S_MONITOR_VOLUME, 0.0);
	cord_micsend_control_defaults(settings);
}

/*
 * Repopulates the device dropdown. Also the "Refresh" button handler.
 *
 * Reads the background cache rather than enumerating here: this runs on the Qt
 * UI thread, and talking to the adb server inline froze the OBS window for
 * seconds every time the source was clicked. See device-cache.h.
 */
static void populate_devices(obs_property_t *list)
{
	obs_property_list_clear(list);

	struct cord_device_list devices;
	/* Bounded, and only ever waits before the first snapshot exists. */
	cord_device_cache_get(&devices, 750);

	if (devices.count == 0) {
		obs_property_list_add_string(
			list,
			obs_module_text(cord_device_cache_is_warm()
						? "Cord.NoDevices"
						: "Cord.Searching"),
			"");
		cord_adb_free_device_list(&devices);
		return;
	}

	for (size_t i = 0; i < devices.count; i++) {
		const struct cord_device *dev = &devices.items[i];

		struct dstr label = {0};
		dstr_copy(&label, dev->model);

		/*
		 * Say why a visible device is unusable, rather than offering it
		 * and failing later.
		 */
		switch (dev->state) {
		case CORD_DEV_UNAUTHORIZED:
			dstr_cat(&label,
				 obs_module_text("Cord.Dev.Unauthorized"));
			break;
		case CORD_DEV_OFFLINE:
			dstr_cat(&label,
				 obs_module_text("Cord.Dev.Offline"));
			break;
		case CORD_DEV_DEVICE:
			/* Wi-Fi works, but it is not what you stream over. */
			if (dev->is_tcpip)
				dstr_cat(&label,
					 obs_module_text("Cord.Dev.WiFi"));
			else
				dstr_cat(&label,
					 obs_module_text("Cord.Dev.Usb"));
			break;
		default:
			break;
		}

		obs_property_list_add_string(list, label.array, dev->serial);
		dstr_free(&label);
	}

	cord_adb_free_device_list(&devices);
}

static bool refresh_clicked(obs_properties_t *props, obs_property_t *button,
			    void *data)
{
	UNUSED_PARAMETER(button);
	UNUSED_PARAMETER(data);

	/* Ask the poller for a fresh pass, then show what we have. */
	cord_device_cache_nudge();

	obs_property_t *list = obs_properties_get(props, S_DEVICE);
	populate_devices(list);
	return true; /* redraw the properties UI */
}

/*
 * Duplication forces the per-app "playback" source, so the source dropdown no
 * longer applies. Hide it rather than letting it look effective.
 */
static bool audio_dup_changed(obs_properties_t *props, obs_property_t *prop,
			      obs_data_t *settings)
{
	UNUSED_PARAMETER(prop);

	const bool dup = obs_data_get_bool(settings, S_AUDIO_DUP);
	obs_property_set_visible(obs_properties_get(props, S_AUDIO_SOURCE),
				 !dup);
	obs_property_set_visible(obs_properties_get(props, "audio_dup_note"),
				 dup);
	return true;
}

/* Shows the monitor's device/level controls only when it is switched on. */
static bool monitor_enabled_changed(obs_properties_t *props,
				    obs_property_t *prop, obs_data_t *settings)
{
	UNUSED_PARAMETER(prop);

	const bool on = obs_data_get_bool(settings, S_MONITOR_ENABLED);
	static const char *const dependent[] = {
		S_MONITOR_DEVICE,
		S_MONITOR_VOLUME,
	};
	for (size_t i = 0; i < sizeof(dependent) / sizeof(dependent[0]); i++)
		obs_property_set_visible(
			obs_properties_get(props, dependent[i]), on);
	return true;
}

/* Per-app capture is Android 13+, so explain rather than silently fail. */
static bool audio_source_changed(obs_properties_t *props, obs_property_t *prop,
				 obs_data_t *settings)
{
	UNUSED_PARAMETER(prop);

	const long long src = obs_data_get_int(settings, S_AUDIO_SOURCE);
	obs_property_t *warning =
		obs_properties_get(props, "audio_source_note");

	obs_property_set_visible(warning,
				 src == CORD_AUDIO_SOURCE_PLAYBACK);
	return true;
}

static obs_properties_t *source_properties(void *data)
{
	UNUSED_PARAMETER(data);

	obs_properties_t *props = obs_properties_create();

	obs_property_t *devices = obs_properties_add_list(
		props, S_DEVICE, obs_module_text("Cord.Device"),
		OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	populate_devices(devices);

	/* button2, not button: the one-arg form is marked OBS_DEPRECATED. */
	obs_properties_add_button2(props, "refresh",
				   obs_module_text("Cord.Refresh"),
				   refresh_clicked, NULL);

	obs_properties_add_text(props, "usb_note",
				obs_module_text("Cord.UsbNote"),
				OBS_TEXT_INFO);

	/* --- video --- */
	obs_property_t *size = obs_properties_add_list(
		props, S_MAX_SIZE, obs_module_text("Cord.MaxSize"),
		OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(size, obs_module_text("Cord.Size.Native"),
				  0);
	obs_property_list_add_int(size, "1920", 1920);
	obs_property_list_add_int(size, "1600", 1600);
	obs_property_list_add_int(size, "1280", 1280);
	obs_property_list_add_int(size, "1024", 1024);

	obs_property_t *fps = obs_properties_add_list(
		props, S_MAX_FPS, obs_module_text("Cord.MaxFps"),
		OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(fps, "120", 120);
	obs_property_list_add_int(fps, "90", 90);
	obs_property_list_add_int(fps, "60", 60);
	obs_property_list_add_int(fps, "30", 30);

	obs_property_t *bitrate = obs_properties_add_int_slider(
		props, S_BITRATE, obs_module_text("Cord.Bitrate"), 2000000,
		40000000, 1000000);
	obs_property_int_set_suffix(bitrate, " bps");

	obs_property_t *codec = obs_properties_add_list(
		props, S_CODEC, obs_module_text("Cord.Codec"),
		OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(codec,
				  obs_module_text("Cord.Codec.H264"),
				  CORD_VIDEO_CODEC_H264);
	obs_property_list_add_int(codec,
				  obs_module_text("Cord.Codec.H265"),
				  CORD_VIDEO_CODEC_H265);
	obs_property_list_add_int(codec, obs_module_text("Cord.Codec.AV1"),
				  CORD_VIDEO_CODEC_AV1);

	/* --- audio --- */
	obs_properties_add_bool(props, S_AUDIO_ENABLED,
				obs_module_text("Cord.AudioEnabled"));

	obs_property_t *asrc = obs_properties_add_list(
		props, S_AUDIO_SOURCE, obs_module_text("Cord.AudioSource"),
		OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(asrc,
				  obs_module_text("Cord.Audio.Output"),
				  CORD_AUDIO_SOURCE_OUTPUT);
	obs_property_list_add_int(asrc,
				  obs_module_text("Cord.Audio.Playback"),
				  CORD_AUDIO_SOURCE_PLAYBACK);
	obs_property_list_add_int(asrc, obs_module_text("Cord.Audio.Mic"),
				  CORD_AUDIO_SOURCE_MIC);
	obs_property_set_modified_callback(asrc, audio_source_changed);

	obs_property_t *note = obs_properties_add_text(
		props, "audio_source_note",
		obs_module_text("Cord.Audio.PlaybackNote"), OBS_TEXT_INFO);
	obs_property_set_visible(note, false);

	/* --- hearing the phone --- */
	obs_properties_add_text(props, "hearing_header",
				obs_module_text("Cord.Hearing.Header"),
				OBS_TEXT_INFO);

	obs_property_t *dup = obs_properties_add_bool(
		props, S_AUDIO_DUP, obs_module_text("Cord.AudioDup"));
	obs_property_set_modified_callback(dup, audio_dup_changed);

	obs_property_t *dup_note = obs_properties_add_text(
		props, "audio_dup_note",
		obs_module_text("Cord.AudioDup.Note"), OBS_TEXT_INFO);
	obs_property_set_visible(dup_note, false);

	obs_property_t *mon = obs_properties_add_bool(
		props, S_MONITOR_ENABLED,
		obs_module_text("Cord.Monitor.Enable"));
	obs_property_set_modified_callback(mon, monitor_enabled_changed);

	obs_property_t *mon_dev = obs_properties_add_list(
		props, S_MONITOR_DEVICE,
		obs_module_text("Cord.Monitor.Device"),
		OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(mon_dev,
				     obs_module_text("Cord.Default"), "");
	{
		struct cord_audio_device_list outs;
		if (cord_list_render_devices(&outs)) {
			for (size_t i = 0; i < outs.count; i++)
				obs_property_list_add_string(
					mon_dev, outs.items[i].name,
					outs.items[i].id);
			cord_audio_device_list_free(&outs);
		}
	}

	obs_property_t *mon_vol = obs_properties_add_float_slider(
		props, S_MONITOR_VOLUME,
		obs_module_text("Cord.Monitor.Volume"), -30.0, 12.0, 0.5);
	obs_property_float_set_suffix(mon_vol, " dB");

	/* --- behaviour --- */
	obs_properties_add_bool(props, S_STAY_AWAKE,
				obs_module_text("Cord.StayAwake"));
	obs_properties_add_bool(props, S_AUTO_RECONNECT,
				obs_module_text("Cord.AutoReconnect"));

	/* --- PC mic -> phone --- */
	cord_micsend_control_add_properties(props);

	return props;
}

struct obs_source_info cord_phone_source_info = {
	.id = "cord_phone",
	.type = OBS_SOURCE_TYPE_INPUT,
	/*
	 * ASYNC_VIDEO because frames arrive on our own threads, AUDIO so the
	 * phone mix is routable to OBS tracks, DO_NOT_DUPLICATE because a
	 * second instance would fight over the same device.
	 */
	.output_flags = OBS_SOURCE_ASYNC_VIDEO | OBS_SOURCE_AUDIO |
			OBS_SOURCE_DO_NOT_DUPLICATE,
	.icon_type = OBS_ICON_TYPE_CAMERA,
	.get_name = source_get_name,
	.create = source_create,
	.destroy = source_destroy,
	.update = source_update,
	.get_defaults = source_defaults,
	.get_properties = source_properties,
	.get_width = source_get_width,
	.get_height = source_get_height,
};
