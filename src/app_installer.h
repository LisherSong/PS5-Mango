#pragma once

int app_install_if_needed(void);

/* Manual home-screen registration, driven by the front-end's "add to home
 * screen" button. Idempotent: returns 0 when the entry already exists. PS5-only
 * (calls sceAppInstUtil*); the caller must guard the call. */
int app_register_manual(void);
