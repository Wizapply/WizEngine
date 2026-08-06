#include "CpuAffinity.h"
#include "Log.h"

#include <cstdio>
#include <thread>
#include <vector>

#ifdef _OPENMP
#include <omp.h>
#endif

#ifdef _WIN32
#include <windows.h>
#else
#include <pthread.h>
#include <sched.h>
#include <unistd.h>
#endif

namespace wizengine {

namespace {

// Bits of `mask`, as a compact human-readable list: consecutive runs collapse
// to "a-b" so a typical contiguous set reads as one range.
std::string describeMask(unsigned long long mask) {
    if (mask == 0) return "any core (OS decides)";
    std::string out = "cores ";
    bool first = true;
    int i = 0;
    while (i < 64) {
        if (!(mask & (1ull << i))) {
            ++i;
            continue;
        }
        int start = i;
        while (i < 64 && (mask & (1ull << i))) ++i;
        const int last = i - 1;
        if (!first) out += ",";
        out += std::to_string(start);
        if (last != start) out += "-" + std::to_string(last);
        first = false;
    }
    return out;
}

}  // namespace

int CoreSet::count() const {
    int n = 0;
    for (int i = 0; i < 64; ++i) {
        if (mask & (1ull << i)) ++n;
    }
    return n;
}

std::string CoreSet::describe() const {
    return describeMask(mask);
}

CoreSet parseCoreSet(const std::string& spec) {
    CoreSet set;
    std::size_t pos = 0;
    while (pos < spec.size()) {
        // Each item is either "n" or "a-b"; separated by commas.
        std::size_t comma = spec.find(',', pos);
        if (comma == std::string::npos) comma = spec.size();
        const std::string item = spec.substr(pos, comma - pos);
        pos = comma + 1;
        if (item.empty()) continue;

        const std::size_t dash = item.find('-');
        try {
            if (dash == std::string::npos) {
                const int n = std::stoi(item);
                if (n >= 0 && n < 64) set.mask |= (1ull << n);
            } else {
                int a = std::stoi(item.substr(0, dash));
                int b = std::stoi(item.substr(dash + 1));
                if (a > b) std::swap(a, b);
                for (int n = a; n <= b && n < 64; ++n) {
                    if (n >= 0) set.mask |= (1ull << n);
                }
            }
        } catch (const std::exception&) {
            LOGW("cpu", "ignoring unparsable core spec '%s'", item.c_str());
        }
    }
    return set;
}

CoreSet availableCores() {
    CoreSet set;
#ifdef _WIN32
    DWORD_PTR processMask = 0, systemMask = 0;
    if (GetProcessAffinityMask(GetCurrentProcess(), &processMask, &systemMask)) {
        set.mask = static_cast<unsigned long long>(processMask);
    }
#else
    cpu_set_t cpus;
    CPU_ZERO(&cpus);
    if (sched_getaffinity(0, sizeof(cpus), &cpus) == 0) {
        for (int i = 0; i < 64 && i < CPU_SETSIZE; ++i) {
            if (CPU_ISSET(i, &cpus)) set.mask |= (1ull << i);
        }
    }
#endif
    if (set.mask == 0) {
        // Fall back to "every core the runtime reports".
        const unsigned hw = std::thread::hardware_concurrency();
        for (unsigned i = 0; i < hw && i < 64; ++i) set.mask |= (1ull << i);
    }
    return set;
}

bool pinCurrentThread(const CoreSet& cores) {
    if (cores.empty()) return true;  // nothing requested
#ifdef _WIN32
    const DWORD_PTR wanted = static_cast<DWORD_PTR>(cores.mask);
    return SetThreadAffinityMask(GetCurrentThread(), wanted) != 0;
#else
    cpu_set_t cpus;
    CPU_ZERO(&cpus);
    for (int i = 0; i < 64 && i < CPU_SETSIZE; ++i) {
        if (cores.mask & (1ull << i)) CPU_SET(i, &cpus);
    }
    return pthread_setaffinity_np(pthread_self(), sizeof(cpus), &cpus) == 0;
#endif
}

int pinOpenMpWorkers(const CoreSet& cores, int threads) {
    if (cores.empty()) return 0;

    // Flatten the mask so worker i can take core list[i].
    std::vector<int> list;
    for (int i = 0; i < 64; ++i) {
        if (cores.mask & (1ull << i)) list.push_back(i);
    }
    if (list.empty()) return 0;
    if (threads < 1) threads = int(list.size());

#ifdef _OPENMP
    omp_set_num_threads(threads);
    // Without this, the runtime may hand out fewer threads than asked for and
    // some cores would never get a worker pinned to them.
    omp_set_dynamic(0);

    int pinned = 0;
#pragma omp parallel reduction(+ : pinned)
    {
        const int id = omp_get_thread_num();
        CoreSet one;
        one.mask = 1ull << list[std::size_t(id) % list.size()];
        if (pinCurrentThread(one)) pinned += 1;
    }
    LOGI("cpu", "pinned %d OpenMP worker(s) across %s", pinned,
         cores.describe().c_str());
    return pinned;
#else
    (void)threads;
    LOGW("cpu",
         "this build has no OpenMP - Chrono::Multicore workers cannot be "
         "pinned");
    return 0;
#endif
}

int coreCountOrAll(const CoreSet& cores) {
    if (!cores.empty()) return cores.count();
    const unsigned hw = std::thread::hardware_concurrency();
    return hw > 0 ? int(hw) : 1;
}

}  // namespace wizengine
