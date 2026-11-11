/*
 * Cord - OBS-side control for the mic -> phone bridge.
 *
 * Keeps the bridge's settings, properties and safety checks out of the source
 * itself, and owns the feedback-loop guard.
 */
#pragma once

#include "wasapi-bridge.h"

#include <obs-module.h>
#include <util/threading.h>

#define CORD_MS_ENABLED "micsend_enabled"
#define CORD_MS_CAPTURE "micsend_capture_device"
#define CORD_MS_RENDER "micsend_render_device"
#define CORD_MS_GAIN "micsend_gain_db"
#define CORD_MS_PTT_MODE "micsend_ptt_mode"

enum cord_ptt_mode {
	CORD_PTT_OFF = 0,     /* always open */
	CORD_PTT_PUSH_TO_TALK, /* muted until the hotkey is held */
	CORD_PTT_PUSH_TO_MUTE, /* open until the hotkey is held */
};

struct cord_micsend_control {
	struct cord_micsend *bridge;
	pthread_mutex_t lock;

	bool enabled;
	char capture_id[512];
	char render_id[512];
	float gain;
	enum cord_ptt_mode ptt_mode;
	bool hotkey_down;

	obs_hotkey_id hotkey;
	obs_source_t *owner;
};

void cord_micsend_control_init(struct cord_micsend_control *ctrl,
			       obs_source_t *owner);
void cord_micsend_control_free(struct cord_micsend_control *ctrl);

/* Adds the "Send PC mic to phone" group to a source's properties. */
void cord_micsend_control_add_properties(obs_properties_t *props);
void cord_micsend_control_defaults(obs_data_t *settings);

/* Applies settings, starting or stopping the bridge as needed. */
void cord_micsend_control_update(struct cord_micsend_control *ctrl,
				 obs_data_t *settings);

/*
 * Checks for a routing mistake that would send phone audio back into the phone,
 * which teammates hear as a howling echo. Returns a human-readable warning to
 * be logged, or NULL when the routing is safe. Caller frees with bfree().
 */
char *cord_micsend_check_feedback(const struct cord_micsend_control *ctrl);
