/* Copyright (c) 2011 Stanford University
 *
 * Permission to use, copy, modify, and distribute this software for any purpose
 * with or without fee is hereby granted, provided that the above copyright
 * notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR(S) DISCLAIM ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL AUTHORS BE LIABLE FOR ANY
 * SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER
 * RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF
 * CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN
 * CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include "BenchUtil.h"
#include "Initialize.h"
#include "ServiceManager.h"

namespace RAMCloud {
/**
 * Default object used to make system calls.
 */
static Syscall defaultSyscall;

ServiceManager* serviceManager = NULL;
static Initialize _(ServiceManager::init);

/**
 * Used by this class to make all system calls.  In normal production
 * use it points to defaultSyscall; for testing it points to a mock
 * object.
 */
Syscall* ServiceManager::sys = &defaultSyscall;

// Length of time that a worker will actively poll for new work before it puts
// itself to sleep. This period should be much longer than typical RPC
// round-trip times so the worker thread doesn't go to sleep in an ongoing
// conversation with a single client.
int ServiceManager::pollMicros = 100;

// The following constant is used to signal a worker thread that
// it should exit.
#define WORKER_EXIT reinterpret_cast<Transport::ServerRpc*>(1)

/**
 * Perform once-only overall initialization for the ServiceManager class, such
 * as creating the global #serviceManager object.  This method is invoked
 * automatically during initialization, but it may be invoked explicitly by
 * other modules to ensure that initialization occurs before those modules
 * initialize themselves.
 */
void
ServiceManager::init() {
    Dispatch::init();
    if (serviceManager == NULL) {
        serviceManager = new ServiceManager();
    }
}

/**
 * Construct a ServiceManager.
 */
ServiceManager::ServiceManager()
    : services(), busyThreads(), idleThreads(), serviceCount(0), extraRpcs()
{
}

/**
 * Destroy a ServiceManager.  This method is most commonly invoked during
 * testing (ServiceManagers rarely, if ever, get deleted in normal operation).
 */
ServiceManager::~ServiceManager()
{
    assert(dispatch->isDispatchThread());
    if (serviceManager == this) {
        serviceManager = NULL;
    }
    while (!busyThreads.empty()) {
        dispatch->poll();
    }
    foreach (Worker* worker, idleThreads) {
        worker->exit();
        delete worker;
    }
}

/**
 * Register a new service with this ServiceManager; from now on, incoming
 * RPCs that specify the service's RpcService value will be dispatched to
 * the service.
 *
 * \param service
 *      The service to add.
 * \param type
 *      Incoming RPCs will be dispatched to the surface if they have this
 *      value in the #service field of their headers.  This type must not
 *      already have been used in a prior call to this method.
 */

void
ServiceManager::addService(Service& service, RpcServiceType type) {
    assert(!services[type]);
    services[type].construct(service);
    serviceCount++;
}

/**
 * Transports invoke this method when an incoming RPC is complete and
 * ready for processing.  This method will arrange for the RPC (eventually)
 * to be serviced, and will invoke its #sendReply method once the RPC
 * has been serviced.
 *
 * \param rpc
 *      RPC object containing a fully-formed request that is ready for
 *      service.
 */
void
ServiceManager::handleRpc(Transport::ServerRpc* rpc)
{
    // Find the service for this RPC.
    const RpcRequestCommon* header;
    header = rpc->requestPayload.getStart<RpcRequestCommon>();
    if ((header == NULL) || (header->service > MAX_SERVICE) ||
            !services[header->service]) {
        if (serviceCount == 0) {
            // Special case for testing.
            extraRpcs.push(rpc);
            return;
        }
        if (header == NULL) {
            LOG(WARNING, "Incoming RPC contains no header (message length %d)",
                    rpc->requestPayload.getTotalLength());
            Service::prepareErrorResponse(rpc->replyPayload,
                    STATUS_MESSAGE_TOO_SHORT);
        } else {
            LOG(WARNING, "Incoming RPC requested unavilable service %d",
                    header->service);
            Service::prepareErrorResponse(rpc->replyPayload,
                    STATUS_SERVICE_NOT_AVAILABLE);
        }
        rpc->sendReply();
        return;
    }
    ServiceInfo* serviceInfo = services[header->service].get();

    // See if we have exceeded the concurrency limit for the service.
    if (serviceInfo->requestsRunning >= serviceInfo->maxThreads) {
        serviceInfo->waitingRpcs.push(rpc);
        return;
    }
    serviceInfo->requestsRunning++;

    // Find a thread to execute the RPC, and hand off the RPC.
    Worker* worker;
    if (idleThreads.empty()) {
        worker = new Worker;
        worker->thread.construct(workerMain, worker);
    } else {
        worker = idleThreads.back();
        idleThreads.pop_back();
    }
    worker->serviceInfo = serviceInfo;
    worker->handoff(rpc);
    worker->busyIndex = downCast<int>(busyThreads.size());
    busyThreads.push_back(worker);
}

/**
 * Returns true if there are currently no RPCs being serviced, false
 * if at least one RPC is currently being executed by a worker.  If true
 * is returned, it also means that any changes to memory made by any
 * worker threads will be visible to the caller.
 */
bool
ServiceManager::idle()
{
    return busyThreads.empty();
}

/**
 * This method is invoked by Dispatch during its polling loop.  It checks
 * for completion of outstanding RPCs.
 */
void
ServiceManager::poll()
{
    // Each iteration of the following loop checks the status of one active
    // worker. The order of iteration is crucial, since it allows us to
    // remove a worker from busyThreads in the middle of the loop without
    // interfering with the remaining iterations.
    for (int i = downCast<int>(busyThreads.size()) - 1; i >= 0; i--) {
        Worker* worker = busyThreads[i];
        assert(worker->busyIndex == i);
        int state = worker->state.load();
        if (state == Worker::WORKING) {
            continue;
        }

        // The worker is either post-processing or idle; in either case, if
        // there is an RPC that we haven't yet responded to, respond now.
        if (worker->rpc != NULL) {
            worker->rpc->sendReply();
            worker->rpc = NULL;
        }

        if (state != Worker::POSTPROCESSING) {
            // If there is work waiting for this service, start the next RPC.
            ServiceInfo* info = worker->serviceInfo;
            if (!info->waitingRpcs.empty()) {
                worker->handoff(info->waitingRpcs.front());
                info->waitingRpcs.pop();
            } else {
                // This worker is now idle; remove it from busyThreads (fill
                // its slot with the worker in the last slot).
                if (worker != busyThreads.back()) {
                    busyThreads[worker->busyIndex] = busyThreads.back();
                    busyThreads[worker->busyIndex]->busyIndex =
                            worker->busyIndex;
                }
                busyThreads.pop_back();
                worker->busyIndex = -1;
                idleThreads.push_back(worker);
                info->requestsRunning--;
            }
        }
    }
}

/**
 * Wait for an RPC request to arrive, but give up if it takes too long.
 * This method is intended only for testing (it assumes that a NULL
 * service was specified in the constructor).
 *
 * \param timeoutSeconds
 *      If a request doesn't arrive within this many seconds, return NULL.
 *
 * \result
 *      The incoming RPC request, or NULL if nothing arrived within the time
 *      limit.
 */
Transport::ServerRpc*
ServiceManager::waitForRpc(double timeoutSeconds) {
    uint64_t start = rdtsc();
    while (true) {
        if (!extraRpcs.empty()) {
            Transport::ServerRpc* result = extraRpcs.front();
            extraRpcs.pop();
            return result;
        }
        if (cyclesToSeconds(rdtsc() - start) > timeoutSeconds) {
            return NULL;
        }
        dispatch->poll();
    }
}

/**
 * This is the top-level method for worker threads.  It repeatedly waits for
 * an RPC to be assigned to it, then executes that RPC and communicates its
 * completion back to the dispatch thread.
 *
 * \param worker
 *      Pointer to information used to communicate between the worker thread
 *      and the dispatch thread.
 */
void
ServiceManager::workerMain(Worker* worker)
try {
    uint64_t pollCycles = nanosecondsToCycles(1000*pollMicros);
    while (true) {
        uint64_t stopPollingTime = dispatch->currentTime + pollCycles;

        // Wait for ServiceManager to supply us with some work to do.
        while (worker->state.load() != Worker::WORKING) {
            if (dispatch->currentTime >= stopPollingTime) {
                // It's been a long time since we've had any work to do; go
                // to sleep so we don't waste any more CPU cycles.  Tricky
                // race condition: the dispatch thread could change the state
                // to WORKING just before we change it to SLEEPING, so use an
                // atomic op and only change to SLEEPING if the current value
                // is POLLING.
                int expected = Worker::POLLING;
                if (worker->state.compareExchange(expected, Worker::SLEEPING)) {
                    if (sys->futexWait(reinterpret_cast<int*>(&worker->state),
                            Worker::SLEEPING) == -1) {
                        // EWOULDBLOCK means that someone already changed
                        // worker->state, so we didn't block; this is benign.
                        if (errno != EWOULDBLOCK) {
                            LOG(ERROR, "futexWait failed in ServiceManager::"
                                    "workerMain: %s", strerror(errno));
                        }
                    }
                }
            }
            // Empty loop body.
        }
        if (worker->rpc == WORKER_EXIT)
            break;

        Service::Rpc rpc(worker, worker->rpc->requestPayload,
                worker->rpc->replyPayload);
        worker->serviceInfo->service.handleRpc(rpc);

        // Pass the RPC back to ServiceManager for completion.
        worker->state.store(Worker::POLLING);
    }
    TEST_LOG("exiting");
} catch (std::exception& e) {
    using namespace RAMCloud;
    LOG(ERROR, "worker: %s", e.what());
    throw; // will likely call std::terminate()
}

/**
 * Force this worker's thread to exit (and don't return until it has exited).
 * This method is only used during testing and ServiceManager destruction.
 * This method should be invoked only in the dispatch thread.
 */
void
Worker::exit()
{
    assert(dispatch->isDispatchThread());
    if (exited) {
        // Worker already exited; nothing to do.  This should only happen
        // during tests.
        return;
    }

    // Wait for the worker thread to finish handling any RPCs already
    // queued for it.
    while (busyIndex >= 0) {
        dispatch->poll();
    }

    // Tell the worker thread to exit, and wait for it to actually exit
    // (don't want it referencing the Worker structure anymore, since
    // it could go away).
    handoff(WORKER_EXIT);
    thread->join();
    rpc = NULL;
    exited = true;
}

/**
 * This method is invoked by the dispatch thread to pass an RPC to an idle
 * worker.  It should only be invoked when the worker is idle (i.e. #rpc is
 * NULL).
 *
 * \param newRpc
 *      RPC object containing a fully-formed request that is ready for
 *      service.
 */
void
Worker::handoff(Transport::ServerRpc* newRpc)
{
    assert(rpc == NULL);
    rpc = newRpc;
    int prevState = state.exchange(WORKING);
    if (prevState == SLEEPING) {
        // The worker got tired of polling and went to sleep, so we
        // have to do extra work to wake it up.
        if (ServiceManager::sys->futexWake(reinterpret_cast<int*>(&state), 1)
                == -1) {
            LOG(ERROR,
                    "futexWake failed in Worker::handoff: %s",
                    strerror(errno));
            // We should probably do something else here, such as panicking
            // or unwinding the RPC.  As of 6/2011 it isn't clear what the
            // right action is, so things just get left in limbo.
        }
    }
}

/**
 * Tell the dispatch thread that this worker has finished processing its RPC,
 * so it is safe to start sending the reply.  This method should only be
 * invoked in the worker thread.
 */
void
Worker::sendReply()
{
    state.store(POSTPROCESSING);
}

} // namespace RAMCloud