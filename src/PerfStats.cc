/* Copyright (c) 2014 Stanford University
 *
 * Permission to use, copy, modify, and distribute this software for any purpose
 * with or without fee is hereby granted, provided that the above copyright
 * notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR(S) DISCLAIM ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL AUTHORS BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include "Cycles.h"
#include "PerfStats.h"

namespace RAMCloud {

SpinLock PerfStats::mutex;
std::vector<PerfStats*> PerfStats::registeredStats;
__thread PerfStats PerfStats::threadStats;

/**
 * This method must be called to make a PerfStats structure "known" so that
 * its contents will be considered by collectStats. Typically this method
 * is invoked once for the thread-local structure associated with each
 * thread. This method is idempotent and thread-safe, so it is safe to
 * invoke it multiple times for the same PerfStats.
 *
 * \param stats
 *      PerfStats structure to remember for usage by collectStats. If this
 *      is the first time this structure has been registered, all of its
 *      counters will be initialized.
 */
void
PerfStats::registerStats(PerfStats* stats)
{
    std::lock_guard<SpinLock> lock(mutex);

    // First see if this structure is already registered; if so,
    // there is nothing for us to do.
    foreach (PerfStats* registered, registeredStats) {
        if (registered == stats) {
            return;
        }
    }

    // This is a new structure; add it to our list, and reset its contents.
    stats->readCount = 0;
    stats->writeCount = 0;
    stats->workerActiveCycles = 0;
    registeredStats.push_back(stats);
}

/**
 * This method aggregates performance information from all of the
 * PerfStats structures that have been registered via the registerStats
 * method.
 * 
 * \param[out] total
 *      Filled in with the sum of all statistics from all registered
 *      PerfStat structures; any existing contents are overwritten.
 */
void
PerfStats::collectStats(PerfStats* total)
{
    std::lock_guard<SpinLock> lock(mutex);
    total->readCount = 0;
    total->writeCount = 0;
    total->workerActiveCycles = 0;
    total->compactorInputBytes = 0;
    total->compactorBytesFreed = 0;
    total->compactorActiveCycles = 0;
    total->cleanerInputMemoryBytes = 0;
    total->cleanerMemoryBytesFreed = 0;
    total->cleanerActiveCycles = 0;
    foreach (PerfStats* stats, registeredStats) {
        total->readCount += stats->readCount;
        total->writeCount += stats->writeCount;
        total->workerActiveCycles += stats->workerActiveCycles;
        total->compactorInputBytes += stats->compactorInputBytes;
        total->compactorBytesFreed += stats->compactorBytesFreed;
        total->compactorActiveCycles += stats->compactorActiveCycles;
        total->cleanerInputMemoryBytes += stats->cleanerInputMemoryBytes;
        total->cleanerMemoryBytesFreed += stats->cleanerMemoryBytesFreed;
        total->cleanerActiveCycles += stats->cleanerActiveCycles;
        total->temp1 += stats->temp1;
        total->temp2 += stats->temp2;
        total->temp3 += stats->temp3;
        total->temp4 += stats->temp4;
        total->temp5 += stats->temp5;
    }
    total->collectionTime = Cycles::rdtsc();
    total->cyclesPerSecond = Cycles::perSecond();
}

}  // namespace RAMCloud
