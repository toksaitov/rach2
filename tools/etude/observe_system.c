/* Native system counters and bounded process snapshots for load accounting. */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef _WIN32
#include <windows.h>
#include <tlhelp32.h>
#else
#include <dirent.h>
#include <unistd.h>
#endif
#ifdef __APPLE__
#include <libproc.h>
#include <mach/mach.h>
#include <sys/proc_info.h>
#endif
#include "observe_system.h"

const char *etude_load_field_name(unsigned field)
{
    static const char *const names[] = {
        "system_busy_cpu_us", "available_memory_bytes", "swap_in_bytes",  "swap_out_bytes",
        "block_read_bytes",   "block_write_bytes",      "memory_stall_us"};
    return field < LOAD_FIELDS ? names[field] : "unknown";
}

#ifdef _WIN32
static uint64_t filetime_value(FILETIME v)
{
    return (uint64_t)v.dwHighDateTime << 32 | v.dwLowDateTime;
}

void etude_load_clock(uint64_t *mono, uint64_t *utc)
{
    LARGE_INTEGER counter, frequency;
    FILETIME now;
    QueryPerformanceCounter(&counter);
    QueryPerformanceFrequency(&frequency);
    *mono = (uint64_t)(counter.QuadPart / frequency.QuadPart) * 1000000u +
            (uint64_t)(counter.QuadPart % frequency.QuadPart) * 1000000u / frequency.QuadPart;
    GetSystemTimePreciseAsFileTime(&now);
    *utc = (filetime_value(now) - UINT64_C(116444736000000000)) / 10;
}

const char *etude_load_platform(void)
{
    return "windows-native-counters-v1";
}

static void windows_counters(struct load_snapshot *s)
{
    FILETIME idle, kernel, user;
    MEMORYSTATUSEX memory;
    memset(&memory, 0, sizeof memory);
    memory.dwLength = sizeof memory;
    if (GetSystemTimes(&idle, &kernel, &user)) {
        s->value[LOAD_CPU] =
            (filetime_value(kernel) - filetime_value(idle) + filetime_value(user)) / 10;
        s->available |= 1u << LOAD_CPU;
    }
    if (GlobalMemoryStatusEx(&memory)) {
        s->value[LOAD_MEMORY] = memory.ullAvailPhys;
        s->available |= 1u << LOAD_MEMORY;
    }
}

static int windows_process(const PROCESSENTRY32W *entry, struct load_process *p)
{
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, entry->th32ProcessID);
    FILETIME created, ended, kernel, user;
    if (!h)
        return 0;
    int ok = GetProcessTimes(h, &created, &ended, &kernel, &user) != 0;
    if (ok) {
        p->id.pid = (long)entry->th32ProcessID;
        p->id.start = filetime_value(created);
        p->id.start_known = 1;
        p->parent = (long)entry->th32ParentProcessID;
        p->cpu_us = (filetime_value(kernel) + filetime_value(user)) / 10;
        p->scope = wcsstr(entry->szExeFile, L"vmmem") || wcsstr(entry->szExeFile, L"Vmmem") ? 3 : 0;
    }
    CloseHandle(h);
    return ok;
}

int etude_load_snapshot(struct load_snapshot *s, struct load_process *p, size_t capacity)
{
    memset(s, 0, sizeof *s);
    etude_load_clock(&s->monotonic_us, &s->utc_us);
    windows_counters(s);
    HANDLE h = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    PROCESSENTRY32W entry;
    memset(&entry, 0, sizeof entry);
    entry.dwSize = sizeof entry;
    if (h == INVALID_HANDLE_VALUE)
        return -1;
    if (Process32FirstW(h, &entry))
        do {
            if (!entry.th32ProcessID)
                continue;
            if (s->count == capacity) {
                s->overflow = 1;
                break;
            }
            if (windows_process(&entry, &p[s->count]))
                s->count++;
            else
                s->denied++;
        } while (Process32NextW(h, &entry));
    CloseHandle(h);
    return 0;
}
#else
void etude_load_clock(uint64_t *mono, uint64_t *utc)
{
    struct timespec m, u;
    clock_gettime(CLOCK_MONOTONIC, &m);
    clock_gettime(CLOCK_REALTIME, &u);
    *mono = (uint64_t)m.tv_sec * 1000000u + m.tv_nsec / 1000u;
    *utc = (uint64_t)u.tv_sec * 1000000u + u.tv_nsec / 1000u;
}

#ifdef __APPLE__
const char *etude_load_platform(void)
{
    return "macos-mach-libproc-v1";
}

static void mac_counters(struct load_snapshot *s)
{
    host_cpu_load_info_data_t cpu = {0};
    vm_statistics64_data_t vm = {0};
    mach_msg_type_number_t n = HOST_CPU_LOAD_INFO_COUNT;
    mach_port_t host = mach_host_self();
    long hz = sysconf(_SC_CLK_TCK), page = sysconf(_SC_PAGESIZE);
    if (hz > 0 &&
        host_statistics(host, HOST_CPU_LOAD_INFO, (host_info_t)&cpu, &n) == KERN_SUCCESS) {
        s->value[LOAD_CPU] = ((uint64_t)cpu.cpu_ticks[CPU_STATE_USER] +
                              cpu.cpu_ticks[CPU_STATE_SYSTEM] + cpu.cpu_ticks[CPU_STATE_NICE]) *
                             1000000u / hz;
        s->available |= 1u << LOAD_CPU;
    }
    n = HOST_VM_INFO64_COUNT;
    if (page > 0 &&
        host_statistics64(host, HOST_VM_INFO64, (host_info64_t)&vm, &n) == KERN_SUCCESS) {
        s->value[LOAD_MEMORY] = ((uint64_t)vm.free_count + vm.inactive_count) * page;
        s->value[LOAD_SWAP_IN] = vm.swapins * page;
        s->value[LOAD_SWAP_OUT] = vm.swapouts * page;
        s->available |= (1u << LOAD_MEMORY) | (1u << LOAD_SWAP_IN) | (1u << LOAD_SWAP_OUT);
    }
    mach_port_deallocate(mach_task_self(), host);
}

int etude_load_snapshot(struct load_snapshot *s, struct load_process *p, size_t capacity)
{
    pid_t *ids = calloc(capacity + 1, sizeof(*ids));
    memset(s, 0, sizeof *s);
    etude_load_clock(&s->monotonic_us, &s->utc_us);
    mac_counters(s);
    if (!ids)
        return -1;
    int bytes = proc_listallpids(ids, (int)((capacity + 1) * sizeof(*ids)));
    if (bytes < 0) {
        free(ids);
        return -1;
    }
    size_t count = (size_t)bytes;
    s->overflow = count > capacity;
    for (size_t i = 0; i < count && i < capacity; i++) {
        struct proc_taskallinfo info;
        if (!ids[i])
            continue;
        if (proc_pidinfo(ids[i], PROC_PIDTASKALLINFO, 0, &info, sizeof info) != sizeof info) {
            s->denied++;
            continue;
        }
        struct load_process *v = &p[s->count++];
        v->id.pid = ids[i];
        v->id.start_known = 1;
        v->id.start = (uint64_t)info.pbsd.pbi_start_tvsec * 1000000u + info.pbsd.pbi_start_tvusec;
        v->parent = info.pbsd.pbi_ppid;
        v->cpu_us = (info.ptinfo.pti_total_user + info.ptinfo.pti_total_system) / 1000;
        v->scope = 0;
    }
    free(ids);
    return 0;
}
#else
const char *etude_load_platform(void)
{
    return "linux-proc-counters-v1";
}

static void linux_memory(struct load_snapshot *s)
{
    char line[256], key[80];
    unsigned long long value;
    long page = sysconf(_SC_PAGESIZE);
    FILE *f = fopen("/proc/meminfo", "r");
    if (f) {
        while (fgets(line, sizeof line, f))
            if (sscanf(line, "MemAvailable: %llu kB", &value) == 1) {
                s->value[LOAD_MEMORY] = value * 1024u;
                s->available |= 1u << LOAD_MEMORY;
            }
        fclose(f);
    }
    f = fopen("/proc/vmstat", "r");
    if (f) {
        while (fscanf(f, "%79s %llu", key, &value) == 2) {
            int field = !strcmp(key, "pswpin")    ? LOAD_SWAP_IN
                        : !strcmp(key, "pswpout") ? LOAD_SWAP_OUT
                                                  : -1;
            if (field >= 0 && page > 0) {
                s->value[field] = value * (uint64_t)page;
                s->available |= 1u << field;
            }
        }
        fclose(f);
    }
    f = fopen("/proc/pressure/memory", "r");
    if (f) {
        if (fgets(line, sizeof line, f)) {
            char *total = strstr(line, "total=");
            if (total && sscanf(total, "total=%llu", &value) == 1) {
                s->value[LOAD_PRESSURE] = value;
                s->available |= 1u << LOAD_PRESSURE;
            }
        }
        fclose(f);
    }
}

static void linux_disk(struct load_snapshot *s)
{
    FILE *f = fopen("/proc/diskstats", "r");
    char line[1024], name[128], path[160];
    unsigned major, minor;
    unsigned long long reads, merged, sectors, ms, writes, wm, ws;
    int found = 0;
    if (!f)
        return;
    while (fgets(line, sizeof line, f)) {
        if (sscanf(line, "%u %u %127s %llu %llu %llu %llu %llu %llu %llu", &major, &minor, name,
                   &reads, &merged, &sectors, &ms, &writes, &wm, &ws) != 10)
            continue;
        if (snprintf(path, sizeof path, "/sys/block/%s", name) >= (int)sizeof path ||
            access(path, F_OK) != 0)
            continue;
        s->value[LOAD_DISK_READ] += sectors * 512u;
        s->value[LOAD_DISK_WRITE] += ws * 512u;
        found = 1;
    }
    fclose(f);
    if (found)
        s->available |= (1u << LOAD_DISK_READ) | (1u << LOAD_DISK_WRITE);
}

static void linux_counters(struct load_snapshot *s)
{
    FILE *f = fopen("/proc/stat", "r");
    unsigned long long u, n, k, idle, wait, irq, soft, steal;
    long hz = sysconf(_SC_CLK_TCK);
    if (f) {
        if (hz > 0 && fscanf(f, "cpu %llu %llu %llu %llu %llu %llu %llu %llu", &u, &n, &k, &idle,
                             &wait, &irq, &soft, &steal) == 8) {
            s->value[LOAD_CPU] = (u + n + k + irq + soft + steal) * 1000000u / hz;
            s->available |= 1u << LOAD_CPU;
        }
        fclose(f);
    }
    linux_memory(s);
    linux_disk(s);
}

static int linux_process(long pid, struct load_process *v)
{
    char path[64], text[4096], *p, *end;
    FILE *f;
    long hz = sysconf(_SC_CLK_TCK);
    snprintf(path, sizeof path, "/proc/%ld/stat", pid);
    f = fopen(path, "r");
    if (!f || hz <= 0) {
        if (f)
            fclose(f);
        return 0;
    }
    size_t n = fread(text, 1, sizeof text - 1, f);
    fclose(f);
    text[n] = 0;
    p = strrchr(text, ')');
    if (!p || p[1] != ' ')
        return 0;
    p += 2;
    memset(v, 0, sizeof *v);
    v->id.pid = pid;
    for (unsigned field = 3; field <= 22; field++) {
        if (field == 4)
            v->parent = strtol(p, NULL, 10);
        if (field == 14 || field == 15)
            v->cpu_us += strtoull(p, NULL, 10);
        if (field == 22) {
            v->id.start = strtoull(p, &end, 10);
            v->id.start_known = end != p;
        }
        p = strchr(p, ' ');
        if (!p && field != 22)
            return 0;
        if (p)
            p++;
    }
    v->cpu_us = v->cpu_us * 1000000u / hz;
    return v->id.start_known;
}

int etude_load_snapshot(struct load_snapshot *s, struct load_process *p, size_t capacity)
{
    struct dirent *entry;
    memset(s, 0, sizeof *s);
    etude_load_clock(&s->monotonic_us, &s->utc_us);
    linux_counters(s);
    DIR *d = opendir("/proc");
    if (!d)
        return -1;
    while ((entry = readdir(d))) {
        char *end;
        long pid = strtol(entry->d_name, &end, 10);
        if (pid <= 0 || *end)
            continue;
        if (s->count == capacity) {
            s->overflow = 1;
            break;
        }
        if (linux_process(pid, &p[s->count]))
            s->count++;
        else
            s->denied++;
    }
    closedir(d);
    return 0;
}
#endif
#endif
