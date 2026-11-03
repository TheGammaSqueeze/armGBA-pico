/* SPDX-License-Identifier: MPL-2.0
 * Copyright (c) 2026 TheGammaSqueeze */
/* armGBA-pico: the render thread (src/video/render-thread.c, the Threaded Video option) is
 * never started on this target: the LK scheduler gives the core one CPU and the frontend never
 * turns the option on. These declarations only let that file compile; pthread_create fails, so
 * any attempt to start the thread falls back to drawing on the calling thread. */
#ifndef ARMGBA_PICO_PTHREAD_H
#define ARMGBA_PICO_PTHREAD_H
#include <stddef.h>
#include <sys/types.h>   /* newlib defines the pthread types (sys/_pthreadtypes.h) */
#ifndef PTHREAD_MUTEX_INITIALIZER
#define PTHREAD_MUTEX_INITIALIZER ((pthread_mutex_t) 0xFFFFFFFF)
#endif
#ifndef PTHREAD_COND_INITIALIZER
#define PTHREAD_COND_INITIALIZER ((pthread_cond_t) 0xFFFFFFFF)
#endif
int pthread_create(pthread_t *t, const pthread_attr_t *a, void *(*fn)(void *), void *arg);
int pthread_join(pthread_t t, void **ret);
int pthread_mutex_init(pthread_mutex_t *m, const pthread_mutexattr_t *a);
int pthread_mutex_destroy(pthread_mutex_t *m);
int pthread_mutex_lock(pthread_mutex_t *m);
int pthread_mutex_unlock(pthread_mutex_t *m);
int pthread_cond_init(pthread_cond_t *c, const pthread_condattr_t *a);
int pthread_cond_destroy(pthread_cond_t *c);
int pthread_cond_wait(pthread_cond_t *c, pthread_mutex_t *m);
int pthread_cond_signal(pthread_cond_t *c);
int pthread_cond_broadcast(pthread_cond_t *c);
#endif
