#include "save_domain.h"

#include <pthread.h>
#include <stdint.h>
#include <sys/types.h>
#include <unistd.h>

#ifndef __linux__
extern int kernel_set_ucred_authid(unsigned long long);
extern int kernel_get_ucred_caps(int pid, uint8_t *caps);
extern int kernel_set_ucred_caps(int pid, const uint8_t *caps);
#endif

#define SAVE_AUTHID 0x4800000000000010ULL

/* The counter is the point of this file -- see save_domain.h. On the console it
 * proves nothing; on the host it is the only observable trace that a code path
 * entered the save domain at all, because the kernel call it stands for does
 * not exist there.
 *
 * Mutex, not a bare ++: save_*_worker run on their own threads. */
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static unsigned long g_enters;

/* Counting is not conditional on the platform -- that is the whole point. */
static void
note_enter(void) {
  pthread_mutex_lock(&g_lock);
  g_enters++;
  pthread_mutex_unlock(&g_lock);
}

int
save_domain_enter(void) {
  note_enter();
#ifdef __linux__
  return 0;
#else
  return kernel_set_ucred_authid(SAVE_AUTHID) ? -1 : 0;
#endif
}

unsigned long
save_domain_enters(void) {
  unsigned long n;

  pthread_mutex_lock(&g_lock);
  n = g_enters;
  pthread_mutex_unlock(&g_lock);
  return n;
}

void
save_domain_reset(void) {
  pthread_mutex_lock(&g_lock);
  g_enters = 0;
  pthread_mutex_unlock(&g_lock);
}

int
save_domain_escalate_full(void) {
#ifdef __linux__
  /* Count even here: on the host nothing below exists, and a test that asks
     "did this path enter the save domain at all" must not be able to tell the
     difference between the two entry points. */
  note_enter();
  return 0;
#else
  uint8_t caps[16];

  note_enter();
  if(kernel_set_ucred_authid(SAVE_AUTHID)) return -1;
  kernel_get_ucred_caps(getpid(), caps);
  caps[7] |= 0x40;
  kernel_set_ucred_caps(getpid(), caps);
  setuid(0);
  return 0;
#endif
}
