/*
 * Cord - pull an Android phone's screen and sound into OBS, and send the
 * PC microphone back to the phone so teammates can hear you.
 *
 * Copyright (C) 2026
 *
 * This program is free software: you can redistribute it and/or modify it under
 * the terms of the GNU General Public License as published by the Free Software
 * Foundation, either version 3 of the License, or (at your option) any later
 * version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE. See the GNU General Public License for details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program. If not, see <https://www.gnu.org/licenses/>.
 *
 * GPLv3 rather than GPLv2: OBS Studio is "GPLv2 or later", and v3 is what makes
 * the bundled Apache-2.0 scrcpy-server compatible. That server is a separate
 * process we speak a socket protocol to - never linked into this binary - so it
 * is aggregation, not a derived work. See docs/licensing.md.
 */

#include "device/adb-client.h"
#include "device/device-cache.h"

#include <obs-module.h>
#include <plugin-support.h>

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("cord", "en-US")

extern struct obs_source_info cord_phone_source_info;

MODULE_EXPORT const char *obs_module_description(void)
{
	return "Mirror an Android phone's screen and audio into OBS, with a "
	       "PC-microphone send back to the phone";
}

MODULE_EXPORT const char *obs_module_name(void)
{
	return "Cord";
}

bool obs_module_load(void)
{
	if (!cord_adb_global_init()) {
		blog(LOG_ERROR,
		     "[cord] could not initialise networking; the plugin "
		     "will not load");
		return false;
	}

	cord_device_cache_init();
	obs_register_source(&cord_phone_source_info);

	blog(LOG_INFO, "[cord] loaded version %s", PLUGIN_VERSION);
	return true;
}

void obs_module_unload(void)
{
	/*
	 * Sources are destroyed by libobs before this runs, so every session
	 * has already torn down its own tunnels and device process.
	 */
	cord_device_cache_shutdown();
	cord_adb_global_shutdown();
	blog(LOG_INFO, "[cord] unloaded");
}
