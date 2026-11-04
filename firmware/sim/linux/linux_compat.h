/* Linux host compatibility for the OTP sim (written for macOS). */
#pragma once
#ifdef __linux__
#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif
#undef unix
#include <pthread.h>
static inline int otp_setname_np(const char *n) { return pthread_setname_np(pthread_self(), n); }
#define pthread_setname_np(n) otp_setname_np(n)
#endif
