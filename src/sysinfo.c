/* SPDX-License-Identifier: AGPL-3.0-only */
/*
 * Process memory sampling: see sysinfo.h. Exactly one sys_rss_bytes() is compiled, selected
 * by the platform macros below. All variants are stateless and safe to call from any thread;
 * each returns 0 when the operating system refuses the query.
 */
#include "sysinfo.h"
#include <stdio.h>

#if defined(__APPLE__)
#include <mach/mach.h>
/* macOS: resident size from the Mach task info of the current task. */
size_t sys_rss_bytes(void) {
    struct mach_task_basic_info info;
    mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
    if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO, (task_info_t)&info, &count) != KERN_SUCCESS) return 0;
    return (size_t)info.resident_size;
}
#elif defined(_WIN32)
#include <windows.h>
#include <psapi.h>
/* Windows: the working set reported by GetProcessMemoryInfo. */
size_t sys_rss_bytes(void) {
    PROCESS_MEMORY_COUNTERS pmc;
    if (!GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof pmc)) return 0;
    return (size_t)pmc.WorkingSetSize;
}
#else
#include <unistd.h>
/* Linux and other Unix: second field of /proc/self/statm (resident pages) times the page size. */
size_t sys_rss_bytes(void) {
    FILE *f = fopen("/proc/self/statm", "r");
    if (!f) return 0;
    long pages = 0, rss = 0;
    if (fscanf(f, "%ld %ld", &pages, &rss) != 2) rss = 0;
    fclose(f);
    return (size_t)rss * (size_t)sysconf(_SC_PAGESIZE);
}
#endif
