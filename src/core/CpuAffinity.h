#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace wizengine {

// Pinning threads to CPU cores.
//
// Why bother: the physics solver runs the same work on every core each step
// and finishes when the SLOWEST one does. If the OS moves a worker to a core
// that is busy encoding video, that step takes longer and every other worker
// waits. Giving physics its own cores, and keeping the render/encode work off
// them, removes that interference. It also stops threads migrating between
// cores, which throws away their warm caches.
//
// Set a mask of zero to leave scheduling to the OS (the default).
struct CoreSet {
    // Bit i set = core i may be used. Empty (0) means "no preference".
    unsigned long long mask = 0;

    bool empty() const { return mask == 0; }
    int count() const;
    std::string describe() const;  // e.g. "cores 0-5" or "cores 0,2,4"
};

// Parses "0-5", "0,2,4", "6-11" or "" (no preference).
CoreSet parseCoreSet(const std::string& spec);

// Cores this process is allowed to run on, as reported by the OS. Respects an
// affinity already applied externally (taskset, start /affinity), so those
// tools keep working.
CoreSet availableCores();

// Restricts the CALLING thread to `cores`. Returns false if the OS refused;
// the caller should carry on regardless - a failed pin costs performance, not
// correctness.
bool pinCurrentThread(const CoreSet& cores);

// Number of cores in the set, or the machine's core count when it is empty.
int coreCountOrAll(const CoreSet& cores);

// Pins the OpenMP worker threads - the ones Chrono::Multicore actually solves
// on - spreading them one per core across `cores`.
//
// Pinning the thread that CALLS into Chrono is not enough. OpenMP workers are
// separate threads, and on Windows a new thread inherits the PROCESS affinity
// mask, not the creating thread's, so they would still roam every core. MSVC
// only implements OpenMP 2.0, which has no OMP_PLACES/OMP_PROC_BIND either;
// the portable way is to enter a parallel region and have each worker pin
// itself, which is what this does.
//
// Call once, before stepping starts. Returns the number of workers pinned (0
// if the build has no OpenMP, or nothing was requested).
int pinOpenMpWorkers(const CoreSet& cores, int threads);

}  // namespace wizengine
