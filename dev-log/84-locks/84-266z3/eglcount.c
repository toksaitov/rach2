/* eglcount: count the EGL contexts a process makes and destroys, as libepoxy
 * looks eglCreateContext and eglDestroyContext up, and log each call. */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

typedef void *(*dlsym_fn)(void *, const char *);
typedef void *(*create_fn)(void *, void *, void *, const int *);
typedef unsigned (*destroy_fn)(void *, void *);
typedef int (*error_fn)(void);

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static create_fn real_create;
static destroy_fn real_destroy;
static error_fn real_error;
static long alive, made, destroyed, failed, peak;
static int out = -1;

/** Return glibc's own dlsym, found by version so as not to find this one. */
static dlsym_fn real_dlsym(void)
{
	static dlsym_fn fn;

	if (!fn)
		fn = (dlsym_fn)dlvsym(RTLD_NEXT, "dlsym", "GLIBC_2.34");
	return fn;
}

/** Append one line, stamped with the wall clock, to EGLCOUNT_OUT.PID. */
static void say(const char *fmt, long a, long b, long c, long d, long e,
		 void *p)
{
	char line[256], name[512];
	struct timespec ts;
	struct tm tm;
	int n;

	if (out < 0) {
		const char *base = getenv("EGLCOUNT_OUT");

		if (!base)
			return;
		snprintf(name, sizeof(name), "%s.%ld", base, (long)getpid());
		out = open(name, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
		if (out < 0)
			return;
	}
	clock_gettime(CLOCK_REALTIME, &ts);
	gmtime_r(&ts.tv_sec, &tm);
	n = snprintf(line, sizeof(line), "%02d:%02d:%02d.%03ld ", tm.tm_hour,
		     tm.tm_min, tm.tm_sec, ts.tv_nsec / 1000000);
	n += snprintf(line + n, sizeof(line) - n, fmt, a, b, c, d, e, p);
	if (write(out, line, n) != n)
		return;
}

static void *wrap_create(void *dpy, void *cfg, void *share, const int *attr)
{
	void *ctx = real_create(dpy, cfg, share, attr);

	pthread_mutex_lock(&lock);
	if (ctx) {
		made++;
		if (++alive > peak)
			peak = alive;
		say("create alive %ld peak %ld made %ld destroyed %ld failed %ld ctx %p\n",
		     alive, peak, made, destroyed, failed, ctx);
	} else {
		failed++;
		say("create FAILED error 0x%lx alive %ld made %ld destroyed %ld failed %ld %p\n",
		     real_error ? (long)real_error() : -1L, alive, made,
		     destroyed, failed, NULL);
	}
	pthread_mutex_unlock(&lock);
	return ctx;
}

static unsigned wrap_destroy(void *dpy, void *ctx)
{
	unsigned ok = real_destroy(dpy, ctx);

	pthread_mutex_lock(&lock);
	if (ok) {
		destroyed++;
		alive--;
	}
	say("destroy alive %ld peak %ld made %ld destroyed %ld ok %ld ctx %p\n",
	     alive, peak, made, destroyed, (long)ok, ctx);
	pthread_mutex_unlock(&lock);
	return ok;
}

/** Hand out the wrappers in place of the two calls, whatever looks them up. */
static void *swap(const char *name, void *fn)
{
	if (!fn || !name)
		return fn;
	if (strcmp(name, "eglCreateContext") == 0) {
		real_create = (create_fn)fn;
		return (void *)wrap_create;
	}
	if (strcmp(name, "eglDestroyContext") == 0) {
		real_destroy = (destroy_fn)fn;
		return (void *)wrap_destroy;
	}
	if (strcmp(name, "eglGetError") == 0 && !real_error)
		real_error = (error_fn)fn;
	return fn;
}

void *dlsym(void *handle, const char *name)
{
	return swap(name, real_dlsym()(handle, name));
}
