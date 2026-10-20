/* history_collect.c: clocks and resource metrics with explicit missing data. */
#define _POSIX_C_SOURCE 200809L
#include "include/etude/history.h"
#include "history_json.h"
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <sys/time.h>
#endif

/** Make an unavailable metric without implying a measured zero. */
static struct etude_measurement unavailable(const char *reason)
{
    struct etude_measurement result;
    memset(&result, 0, sizeof(result));
    result.reason = reason;
    return result;
}

/** Make a non-sampled metric with a known or unknown error bound. */
static struct etude_measurement measured(uint64_t value, const char *method, const char *quality,
                                         uint64_t error, int known)
{
    struct etude_measurement result;
    memset(&result, 0, sizeof(result));
    result.value = value;
    result.method = method;
    result.quality = quality;
    result.error_bound = error;
    result.error_known = known;
    return result;
}

/** Convert seconds and fractional units without losing the 53-bit bound. */
static int microseconds(uint64_t seconds, uint64_t fraction, uint64_t *out)
{
    if (fraction >= 1000000 || seconds > (ETUDE_HISTORY_MAX - fraction) / 1000000)
        return -1;
    *out = seconds * 1000000 + fraction;
    return 0;
}

/** Read monotonic and UTC clocks independently. */
int etude_history_now(struct etude_reading *reading)
{
    unsigned fraction;
    int length;
#ifdef _WIN32
    LARGE_INTEGER ticks, frequency;
    FILETIME filetime;
    SYSTEMTIME utc;
    uint64_t count, hz, remainder, scaled;
    if (!QueryPerformanceCounter(&ticks) || !QueryPerformanceFrequency(&frequency) ||
        ticks.QuadPart < 0 || frequency.QuadPart <= 0)
        return -1;
    count = (uint64_t)ticks.QuadPart;
    hz = (uint64_t)frequency.QuadPart;
    remainder = count % hz;
    if (remainder > UINT64_MAX / 1000000)
        return -1;
    scaled = remainder * 1000000 / hz;
    if (microseconds(count / hz, scaled, &reading->monotonic_us))
        return -1;
    reading->resolution_us = 1000000 / hz + (1000000 % hz != 0);
    GetSystemTimePreciseAsFileTime(&filetime);
    if (!FileTimeToSystemTime(&filetime, &utc))
        return -1;
    fraction = (unsigned)((((uint64_t)filetime.dwHighDateTime << 32) | filetime.dwLowDateTime) %
                          10000000 / 10);
    length =
        snprintf(reading->utc, sizeof(reading->utc), "%04u-%02u-%02uT%02u:%02u:%02u.%06uZ",
                 utc.wYear, utc.wMonth, utc.wDay, utc.wHour, utc.wMinute, utc.wSecond, fraction);
#else
    struct timespec monotonic, utc, resolution;
    struct tm broken;
    char seconds[21];
    if (clock_gettime(CLOCK_MONOTONIC, &monotonic) || clock_gettime(CLOCK_REALTIME, &utc) ||
        clock_getres(CLOCK_MONOTONIC, &resolution) || monotonic.tv_sec < 0 ||
        !gmtime_r(&utc.tv_sec, &broken))
        return -1;
    if (microseconds((uint64_t)monotonic.tv_sec, (uint64_t)monotonic.tv_nsec / 1000,
                     &reading->monotonic_us) ||
        microseconds((uint64_t)resolution.tv_sec, ((uint64_t)resolution.tv_nsec + 999) / 1000,
                     &reading->resolution_us))
        return -1;
    if (!reading->resolution_us)
        reading->resolution_us = 1;
    if (!strftime(seconds, sizeof(seconds), "%Y-%m-%dT%H:%M:%S", &broken))
        return -1;
    fraction = (unsigned)utc.tv_nsec / 1000;
    length = snprintf(reading->utc, sizeof(reading->utc), "%s.%06uZ", seconds, fraction);
#endif
    if (length != 27 || !eh_utc(reading->utc))
        return -1;
    return 0;
}

/** Keep missing, backwards and overflowing intervals out of measured values. */
struct etude_measurement etude_history_elapsed(const struct etude_reading *start,
                                               const struct etude_reading *end)
{
    uint64_t resolution;
    if (!start || !end)
        return unavailable("the end reading was not observed");
    if (end->monotonic_us < start->monotonic_us || end->monotonic_us > ETUDE_HISTORY_MAX ||
        !start->resolution_us || !end->resolution_us || start->resolution_us > ETUDE_HISTORY_MAX ||
        end->resolution_us > ETUDE_HISTORY_MAX)
        return unavailable("the clock readings are invalid or overflowed");
    resolution =
        start->resolution_us > end->resolution_us ? start->resolution_us : end->resolution_us;
    if (resolution > (ETUDE_HISTORY_MAX - 2) / 2)
        return unavailable("the clock error bound overflowed");
    return measured(end->monotonic_us - start->monotonic_us, "monotonic-us-v1", "exact",
                    resolution * 2 + 2, 1);
}

/** Escape a reason as JSON, including control characters. */
static int quote(const char *text, char *out, size_t size)
{
    size_t used = 0;
    const unsigned char *p = (const unsigned char *)text;
    if (size < 3)
        return -1;
    out[used++] = '"';
    for (; *p; p++) {
        if (*p > 127)
            return -1;
        if (*p < 32 || *p == '"' || *p == '\\') {
            if (size - used < 7)
                return -1;
            snprintf(out + used, size - used, "\\u%04x", (unsigned)*p);
            used += 6;
        } else {
            if (size - used < 3)
                return -1;
            out[used++] = (char)*p;
        }
    }
    out[used++] = '"';
    out[used] = 0;
    return 0;
}

/** Serialize the schema's measured and unavailable alternatives. */
int etude_measurement_json(const struct etude_measurement *m, char *out, size_t size)
{
    char reason[2048], interval[32], error[32];
    int length;
    if (m->reason) {
        if (!*m->reason || quote(m->reason, reason, sizeof(reason)))
            return -1;
        length = snprintf(out, size, "{\"value\":null,\"reason\":%s}", reason);
    } else {
        if (!eh_identity(m->method) || !m->quality ||
            (strcmp(m->quality, "exact") && strcmp(m->quality, "sampled") &&
             strcmp(m->quality, "lower_bound")) ||
            m->value > ETUDE_HISTORY_MAX || m->interval_us > ETUDE_HISTORY_MAX ||
            (m->error_known && m->error_bound > ETUDE_HISTORY_MAX) ||
            (!strcmp(m->quality, "sampled") && !m->interval_us))
            return -1;
        if (m->interval_us)
            snprintf(interval, sizeof(interval), "%llu", (unsigned long long)m->interval_us);
        else
            strcpy(interval, "null");
        if (m->error_known)
            snprintf(error, sizeof(error), "%llu", (unsigned long long)m->error_bound);
        else
            strcpy(error, "null");
        length = snprintf(out, size,
                          "{\"value\":%llu,\"method\":\"%s\",\"quality\":\"%s\","
                          "\"interval_us\":%s,\"error_bound\":%s}",
                          (unsigned long long)m->value, m->method, m->quality, interval, error);
    }
    return length < 0 || (size_t)length >= size ? -1 : 0;
}

/** Sum disjoint accounting, preserving missing data. */
struct etude_measurement etude_history_cpu_sum(const struct etude_measurement *metrics,
                                               size_t count)
{
    struct etude_measurement sum = measured(0, "disjoint-cpu-sum-v1", "exact", 0, 1);
    size_t i;
    if (!count)
        return unavailable("no processor accounting scopes were observed");
    for (i = 0; i < count; i++) {
        const struct etude_measurement *m = &metrics[i];
        char json[1024];
        if (m->reason || etude_measurement_json(m, json, sizeof(json)) ||
            !strcmp(m->quality, "sampled"))
            return unavailable("a processor accounting scope is unavailable");
        if (m->value > ETUDE_HISTORY_MAX - sum.value)
            return unavailable("summed processor time overflowed");
        sum.value += m->value;
        if (strcmp(m->quality, "exact"))
            sum.quality = "lower_bound";
        if (!m->error_known || m->error_bound > ETUDE_HISTORY_MAX - sum.error_bound)
            sum.error_known = 0;
        else
            sum.error_bound += m->error_bound;
        if (m->interval_us > sum.interval_us)
            sum.interval_us = m->interval_us;
    }
    return sum;
}

/** Record simultaneous resident samples, preserving missing data. */
void etude_history_memory_sample(struct etude_measurement *peak,
                                 const struct etude_measurement *resident, size_t count,
                                 uint64_t interval_us)
{
    uint64_t total = 0;
    size_t i;
    if (peak->reason && strcmp(peak->reason, "no resident samples were observed"))
        return;
    if (!count || !interval_us || interval_us > ETUDE_HISTORY_MAX) {
        *peak = unavailable("the resident sampling interval or scope is unavailable");
        return;
    }
    for (i = 0; i < count; i++) {
        if (resident[i].reason || resident[i].value > ETUDE_HISTORY_MAX - total ||
            !resident[i].quality || strcmp(resident[i].quality, "exact")) {
            *peak = unavailable("a resident sample is unavailable or overflowed");
            return;
        }
        total += resident[i].value;
    }
    if (peak->reason || total > peak->value)
        peak->value = total;
    peak->reason = NULL;
    peak->method = "simultaneous-resident-samples-v1";
    peak->quality = "sampled";
    peak->error_known = 0;
    if (interval_us > peak->interval_us)
        peak->interval_us = interval_us;
}

/** Start with explicit absence of accounting, rather than measured zeros. */
void etude_history_accounting_init(struct etude_accounting *accounting)
{
    accounting->cpu = unavailable("no processor accounting scopes were observed");
    accounting->memory = unavailable("no resident samples were observed");
}

#ifndef _WIN32
/** Convert final wait usage while retaining its descendant-accounting limit. */
struct etude_accounting etude_history_wait_usage(const struct rusage *usage)
{
    struct etude_accounting result;
    uint64_t user, system;
    etude_history_accounting_init(&result);
    result.memory = unavailable("wait usage does not report combined resident memory");
    if (!usage || usage->ru_utime.tv_sec < 0 || usage->ru_stime.tv_sec < 0 ||
        usage->ru_utime.tv_usec < 0 || usage->ru_stime.tv_usec < 0 ||
        microseconds((uint64_t)usage->ru_utime.tv_sec, (uint64_t)usage->ru_utime.tv_usec, &user) ||
        microseconds((uint64_t)usage->ru_stime.tv_sec, (uint64_t)usage->ru_stime.tv_usec,
                     &system) ||
        user > ETUDE_HISTORY_MAX - system)
        return result;
    result.cpu = measured(user + system, "wait-reaped-descendants-v1", "lower_bound", 0, 0);
    return result;
}
#else
/** Query inclusive CPU accounting of a non-breakaway owned Job Object. */
struct etude_accounting etude_history_job_usage(void *job)
{
    JOBOBJECT_BASIC_ACCOUNTING_INFORMATION info;
    struct etude_accounting result;
    uint64_t user, kernel;
    etude_history_accounting_init(&result);
    result.memory = unavailable("job peak commit is not combined resident memory");
    if (!QueryInformationJobObject((HANDLE)job, JobObjectBasicAccountingInformation, &info,
                                   sizeof(info), NULL) ||
        info.TotalUserTime.QuadPart < 0 || info.TotalKernelTime.QuadPart < 0)
        return result;
    user = (uint64_t)info.TotalUserTime.QuadPart;
    kernel = (uint64_t)info.TotalKernelTime.QuadPart;
    if (user > UINT64_MAX - kernel || (user + kernel) / 10 > ETUDE_HISTORY_MAX)
        return result;
    result.cpu = measured((user + kernel) / 10, "win32-job-cpu-v1", "exact", 1, 1);
    return result;
}
#endif
