#pragma once

/* The one way into the save domain.
 *
 * Touching a PS5 save (/user/home/<account-hex>/savedata_prospero/...) needs
 * the save AuthID: without it opendir() on the account home simply fails. The
 * AuthID lives in the *thread's* ucred, so every worker thread must set it
 * itself -- a caller having done it proves nothing.
 *
 * That rule used to be enforced by a test grepping src/mango_port.c for the
 * string `save_escalate();` inside a 6000-character window after the function
 * name. It was written that way because on the host the escalate call is
 * `#ifdef __linux__ return 0`, so no behavioural test can observe a missing
 * one -- and the bug ("backup works, but the same save lists as absent",
 * because only one of the two paths escalated) had already happened three
 * times. But the invariant then lived in the *source formatting*: rename the
 * function, wrap the call, split the body, and the guard goes green while the
 * bug comes back.
 *
 * So the count is the interface. save_domain_enter() counts on EVERY platform,
 * including the host where the kernel call is a no-op, and save_domain_enters()
 * makes that observable. The invariant is now "this request entered the save
 * domain, on the request thread AND on the worker thread", asserted against a
 * counter instead of against the shape of the source.
 *
 * save_domain_reset() exists so a test can measure a delta.
 *
 * ⚠️ Do not add a per-thread count here. It was tried and removed on
 * 2026-10-16: pthread_self() values get recycled after a thread exits, so
 * "how many distinct threads escalated" does not reliably grow when a worker
 * escalates for the first time. The delta on `enters` is the reliable signal:
 * a delete costs exactly two (the API path, then the worker), so one is the
 * missing-worker bug and zero is the missing-everything bug.
 */
int save_domain_enter(void);

/* Times the save domain has been entered since start (or reset). */
unsigned long save_domain_enters(void);

void save_domain_reset(void);

/* The deeper step: save AuthID *plus* the caps bit plus setuid(0).
 *
 * savemgr.c used to carry this as a private `static void escalate()` in its
 * `#ifndef __linux__` block -- same AuthID, more privileges, and a second copy
 * of the constant. It is console-only (kernel_get_ucred_caps and friends do
 * not exist on the host), so no host test can exercise it; the reason to move
 * it is that "which AuthID does the save domain use" then has exactly one
 * answer in the tree, and .build/chk_save_domain.py can keep it that way.
 *
 * Returns 0 on success, -1 if the AuthID could not be set. */
int save_domain_escalate_full(void);
