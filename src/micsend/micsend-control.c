#include "micsend-control.h"

#include <util/dstr.h>
#include <util/platform.h>

#include <math.h>
#include <string.h>

#define LOG(level, fmt, ...) \
	blog(level, "[cord/micsend] " fmt, ##__VA_ARGS__)

static float db_to_linear(double db)
{
	/* Treat the bottom of the slider as a hard mute. */
	if (db <= -30.0)
		return 0.0f;
	return (float)pow(10.0, db / 20.0);
}

/* ------------------------------------------------------------------ */
/* hotkey                                                             */
/* ------------------------------------------------------------------ */

static void apply_mute_state(struct cord_micsend_control *ctrl)
{
	if (!ctrl->bridge)
		return;

	bool muted;
	switch (ctrl->ptt_mode) {
	case CORD_PTT_PUSH_TO_TALK:
		muted = !ctrl->hotkey_down;
		break;
	case CORD_PTT_PUSH_TO_MUTE:
		muted = ctrl->hotkey_down;
		break;
	default:
		muted = false;
		break;
	}

	cord_micsend_set_muted(ctrl->bridge, muted);
}

static void hotkey_pressed(void *data, obs_hotkey_id id, obs_hotkey_t *hotkey,
			   bool pressed)
{
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);

	struct cord_micsend_control *ctrl = data;

	pthread_mutex_lock(&ctrl->lock);
	ctrl->hotkey_down = pressed;
	apply_mute_state(ctrl);
	pthread_mutex_unlock(&ctrl->lock);
}

void cord_micsend_control_init(struct cord_micsend_control *ctrl,
			       obs_source_t *owner)
{
	memset(ctrl, 0, sizeof(*ctrl));
	pthread_mutex_init(&ctrl->lock, NULL);
	ctrl->owner = owner;
	ctrl->gain = 1.0f;
	ctrl->hotkey = OBS_INVALID_HOTKEY_ID;

	/*
	 * Registered per source so it appears in OBS's own Settings > Hotkeys
	 * alongside the source, which is where people look for it.
	 */
	ctrl->hotkey = obs_hotkey_register_source(
		owner, "Cord.MicSend.PTT",
		obs_module_text("Cord.MicSend.PTTHotkey"), hotkey_pressed,
		ctrl);
}

void cord_micsend_control_free(struct cord_micsend_control *ctrl)
{
	if (ctrl->hotkey != OBS_INVALID_HOTKEY_ID)
		obs_hotkey_unregister(ctrl->hotkey);

	pthread_mutex_lock(&ctrl->lock);
	cord_micsend_destroy(ctrl->bridge);
	ctrl->bridge = NULL;
	pthread_mutex_unlock(&ctrl->lock);

	pthread_mutex_destroy(&ctrl->lock);
}

/* ------------------------------------------------------------------ */
/* properties                                                          */
/* ------------------------------------------------------------------ */

static void fill_device_list(obs_property_t *list, bool capture)
{
	obs_property_list_clear(list);

	/*
	 * The capture side may sensibly default to the system microphone. The
	 * render side must NOT: an empty render id used to mean "communications
	 * default", which is the user's own headset, so enabling the bridge
	 * just played their microphone back at them and sent nothing to the
	 * phone. The output has to be the device physically wired to the phone,
	 * so it is an explicit, deliberate choice.
	 */
	obs_property_list_add_string(
		list,
		obs_module_text(capture ? "Cord.Default"
					: "Cord.MicSend.PickOutput"),
		"");

	struct cord_audio_device_list devices;
	const bool ok = capture ? cord_list_capture_devices(&devices)
				: cord_list_render_devices(&devices);
	if (!ok)
		return;

	for (size_t i = 0; i < devices.count; i++) {
		/*
		 * Label the PC's own default output in the list. It is the one
		 * device that is certainly NOT the phone, and picking it is the
		 * mistake everyone makes first: you hear your own voice and the
		 * phone receives nothing.
		 */
		if (!capture && devices.items[i].is_default) {
			struct dstr label = {0};
			dstr_printf(&label, "%s%s", devices.items[i].name,
				    obs_module_text(
					    "Cord.MicSend.IsDefaultOutput"));
			obs_property_list_add_string(list, label.array,
						     devices.items[i].id);
			dstr_free(&label);
			continue;
		}

		obs_property_list_add_string(list, devices.items[i].name,
					     devices.items[i].id);
	}

	cord_audio_device_list_free(&devices);
}

/* True when `id` is this PC's own default playback endpoint. */
static bool is_default_render_device(const char *id)
{
	if (!id || !*id)
		return false;

	struct cord_audio_device_list devices;
	if (!cord_list_render_devices(&devices))
		return false;

	bool match = false;
	for (size_t i = 0; i < devices.count; i++) {
		if (devices.items[i].is_default &&
		    strcmp(devices.items[i].id, id) == 0) {
			match = true;
			break;
		}
	}

	cord_audio_device_list_free(&devices);
	return match;
}

/*
 * Surfaces the "you picked your own speakers" warning in the dialog. The log
 * warning was there already, but nobody reads the log while ticking a checkbox.
 */
static bool micsend_render_changed(obs_properties_t *props,
				   obs_property_t *prop, obs_data_t *settings)
{
	UNUSED_PARAMETER(prop);

	const char *render = obs_data_get_string(settings, CORD_MS_RENDER);
	const bool enabled = obs_data_get_bool(settings, CORD_MS_ENABLED);

	obs_property_set_visible(
		obs_properties_get(props, "micsend_default_warning"),
		enabled && is_default_render_device(render));

	return true;
}

static bool micsend_enabled_changed(obs_properties_t *props,
				    obs_property_t *prop, obs_data_t *settings)
{
	UNUSED_PARAMETER(prop);

	const bool on = obs_data_get_bool(settings, CORD_MS_ENABLED);

	static const char *const dependent[] = {
		CORD_MS_CAPTURE, CORD_MS_RENDER, CORD_MS_GAIN,
		CORD_MS_PTT_MODE, "micsend_wiring_note",
	};

	for (size_t i = 0; i < sizeof(dependent) / sizeof(dependent[0]); i++)
		obs_property_set_visible(obs_properties_get(props,
							    dependent[i]),
					 on);

	/* Conditional on the chosen device too, so re-evaluate rather than
	 * blanket-toggling it with the rest. */
	const char *render = obs_data_get_string(settings, CORD_MS_RENDER);
	obs_property_set_visible(
		obs_properties_get(props, "micsend_default_warning"),
		on && is_default_render_device(render));

	return true;
}

void cord_micsend_control_add_properties(obs_properties_t *props)
{
	obs_property_t *enabled = obs_properties_add_bool(
		props, CORD_MS_ENABLED,
		obs_module_text("Cord.MicSend.Enable"));
	obs_property_set_modified_callback(enabled, micsend_enabled_changed);

	obs_properties_add_text(props, "micsend_wiring_note",
				obs_module_text("Cord.MicSend.WiringNote"),
				OBS_TEXT_INFO);

	obs_property_t *cap = obs_properties_add_list(
		props, CORD_MS_CAPTURE,
		obs_module_text("Cord.MicSend.Capture"),
		OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	fill_device_list(cap, true);

	obs_property_t *ren = obs_properties_add_list(
		props, CORD_MS_RENDER,
		obs_module_text("Cord.MicSend.Render"),
		OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	fill_device_list(ren, false);
	obs_property_set_modified_callback(ren, micsend_render_changed);

	obs_property_t *default_warning = obs_properties_add_text(
		props, "micsend_default_warning",
		obs_module_text("Cord.MicSend.DefaultWarning"),
		OBS_TEXT_INFO);
	obs_property_text_set_info_type(default_warning, OBS_TEXT_INFO_WARNING);
	obs_property_set_visible(default_warning, false);

	obs_property_t *gain = obs_properties_add_float_slider(
		props, CORD_MS_GAIN, obs_module_text("Cord.MicSend.Gain"),
		-30.0, 12.0, 0.5);
	obs_property_float_set_suffix(gain, " dB");

	obs_property_t *ptt = obs_properties_add_list(
		props, CORD_MS_PTT_MODE,
		obs_module_text("Cord.MicSend.PTTMode"),
		OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(ptt, obs_module_text("Cord.PTT.Off"),
				  CORD_PTT_OFF);
	obs_property_list_add_int(ptt, obs_module_text("Cord.PTT.Talk"),
				  CORD_PTT_PUSH_TO_TALK);
	obs_property_list_add_int(ptt, obs_module_text("Cord.PTT.Mute"),
				  CORD_PTT_PUSH_TO_MUTE);
}

void cord_micsend_control_defaults(obs_data_t *settings)
{
	obs_data_set_default_bool(settings, CORD_MS_ENABLED, false);
	obs_data_set_default_string(settings, CORD_MS_CAPTURE, "");
	obs_data_set_default_string(settings, CORD_MS_RENDER, "");
	obs_data_set_default_double(settings, CORD_MS_GAIN, 0.0);
	obs_data_set_default_int(settings, CORD_MS_PTT_MODE, CORD_PTT_OFF);
}

/* ------------------------------------------------------------------ */
/* feedback guard                                                      */
/* ------------------------------------------------------------------ */

struct feedback_scan {
	const char *render_id;
	struct dstr warning;
};

/*
 * Looks for a desktop-audio source capturing the very device we send down the
 * cable. That is not a howling loop by itself, but it puts the streamer's own
 * voice into the stream a second time, delayed - which sounds broken on the
 * VOD and is hard to diagnose after the fact.
 */
static bool scan_source(void *param, obs_source_t *source)
{
	struct feedback_scan *scan = param;

	const char *id = obs_source_get_id(source);
	if (!id || strcmp(id, "wasapi_output_capture") != 0)
		return true;

	obs_data_t *settings = obs_source_get_settings(source);
	const char *device = obs_data_get_string(settings, "device_id");

	if (device && *device && strcmp(device, scan->render_id) == 0) {
		dstr_catf(&scan->warning,
			  "- the source '%s' captures the same device this "
			  "bridge sends to, so your voice will be duplicated "
			  "in the stream\n",
			  obs_source_get_name(source));
	}

	obs_data_release(settings);
	return true;
}

char *cord_micsend_check_feedback(const struct cord_micsend_control *ctrl)
{
	if (!ctrl->enabled)
		return NULL;

	/* An empty id means "communications default", which we cannot compare. */
	if (!ctrl->render_id[0])
		return NULL;

	struct feedback_scan scan = {0};
	scan.render_id = ctrl->render_id;

	/*
	 * The real loop: OBS monitoring points at the device wired to the
	 * phone. Anything monitored - the phone's own audio above all - is then
	 * played back into the phone's mic input, and teammates hear
	 * themselves. This is the mistake worth catching.
	 */
	/*
	 * libobs owns these strings; they must not be freed. The getter returns
	 * void, so the id has to be null-checked rather than the call.
	 */
	const char *mon_name = NULL;
	const char *mon_id = NULL;
	obs_get_audio_monitoring_device(&mon_name, &mon_id);

	if (mon_id && strcmp(mon_id, ctrl->render_id) == 0) {
		const enum obs_monitoring_type mt =
			obs_source_get_monitoring_type(ctrl->owner);
		if (mt != OBS_MONITORING_TYPE_NONE) {
			dstr_catf(&scan.warning,
				  "- this source's audio is being monitored to "
				  "'%s', the same device wired to the phone, so "
				  "phone audio is being fed back into the phone "
				  "(teammates will hear an echo)\n",
				  mon_name ? mon_name : "(unknown)");
		} else {
			dstr_catf(&scan.warning,
				  "- OBS monitoring is set to '%s', the same "
				  "device wired to the phone; do not enable "
				  "monitoring on any source while this bridge "
				  "is running\n",
				  mon_name ? mon_name : "(unknown)");
		}
	}

	/*
	 * Sending to the machine's own default playback device almost always
	 * means the user hears themselves and the phone gets nothing - the
	 * exact symptom that makes people think the feature is broken.
	 */
	struct cord_audio_device_list renders;
	if (cord_list_render_devices(&renders)) {
		for (size_t i = 0; i < renders.count; i++) {
			if (!renders.items[i].is_default)
				continue;
			if (strcmp(renders.items[i].id, ctrl->render_id) != 0)
				continue;

			dstr_catf(&scan.warning,
				  "- '%s' is this PC's default playback device, "
				  "so you will hear your own microphone in it "
				  "and the phone will receive nothing. Choose "
				  "the separate output that is wired to the "
				  "phone (see docs/mic-wiring.md)\n",
				  renders.items[i].name);
			break;
		}
		cord_audio_device_list_free(&renders);
	}

	obs_enum_sources(scan_source, &scan);

	if (scan.warning.len == 0) {
		dstr_free(&scan.warning);
		return NULL;
	}

	return scan.warning.array; /* caller bfree()s */
}

/* ------------------------------------------------------------------ */
/* update                                                              */
/* ------------------------------------------------------------------ */

void cord_micsend_control_update(struct cord_micsend_control *ctrl,
				 obs_data_t *settings)
{
	const bool enabled = obs_data_get_bool(settings, CORD_MS_ENABLED);
	const char *capture = obs_data_get_string(settings, CORD_MS_CAPTURE);
	const char *render = obs_data_get_string(settings, CORD_MS_RENDER);
	const double gain_db = obs_data_get_double(settings, CORD_MS_GAIN);
	const enum cord_ptt_mode ptt =
		(enum cord_ptt_mode)obs_data_get_int(settings,
						     CORD_MS_PTT_MODE);

	pthread_mutex_lock(&ctrl->lock);

	const bool devices_changed =
		strcmp(ctrl->capture_id, capture ? capture : "") != 0 ||
		strcmp(ctrl->render_id, render ? render : "") != 0;

	snprintf(ctrl->capture_id, sizeof(ctrl->capture_id), "%s",
		 capture ? capture : "");
	snprintf(ctrl->render_id, sizeof(ctrl->render_id), "%s",
		 render ? render : "");
	ctrl->gain = db_to_linear(gain_db);
	ctrl->ptt_mode = ptt;

	const bool was_enabled = ctrl->enabled;
	ctrl->enabled = enabled;

	/* Restart only when it actually needs restarting. */
	if (ctrl->bridge && (!enabled || devices_changed)) {
		cord_micsend_destroy(ctrl->bridge);
		ctrl->bridge = NULL;
	}

	/*
	 * Refuse to run without an explicitly chosen output. Falling back to
	 * the default device would route the microphone to the user's own
	 * headset - they hear themselves, and the phone hears nothing.
	 */
	if (enabled && !ctrl->render_id[0]) {
		pthread_mutex_unlock(&ctrl->lock);
		LOG(LOG_WARNING,
		    "mic send is enabled but no output device is chosen. Pick "
		    "the output that is wired to the phone - leaving it unset "
		    "would send your microphone to your own headset instead. "
		    "See docs/mic-wiring.md.");
		return;
	}

	if (enabled && !ctrl->bridge) {
		ctrl->bridge = cord_micsend_create(ctrl->capture_id,
						   ctrl->render_id);
	}

	if (ctrl->bridge) {
		cord_micsend_set_gain(ctrl->bridge, ctrl->gain);
		apply_mute_state(ctrl);
	}

	pthread_mutex_unlock(&ctrl->lock);

	/*
	 * Warn on enable, not on every settings touch: this is advice, not an
	 * error, and repeating it on each keystroke would be noise.
	 */
	if (enabled && !was_enabled) {
		char *warning = cord_micsend_check_feedback(ctrl);
		if (warning) {
			LOG(LOG_WARNING,
			    "audio routing problem detected:\n%s", warning);
			bfree(warning);
		}
	}
}
