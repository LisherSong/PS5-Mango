#pragma once

int app_install_if_needed(void);

/* Cheap, non-blocking half of app_install_if_needed(): registers the icon with
 * the asset table only. main() calls this before the listener comes up and
 * defers the (potentially blocking, PS5 installer API) rest to a background
 * thread — a stalled installer call must never keep the web UI down. */
void app_register_assets(void);

/* Manual home-screen registration, driven by the front-end's "add to home
 * screen" button. Idempotent: returns 0 when the entry already exists. PS5-only
 * (calls sceAppInstUtil*); the caller must guard the call. */
int app_register_manual(void);
