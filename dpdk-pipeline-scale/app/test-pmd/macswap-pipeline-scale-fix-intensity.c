// 11KB footprint
// ====================== INSTRUCTION MIX ANALYSIS ======================
// Total Instructions: 48310072465

// INSTRUCTION MIX BREAKDOWN:
// -------------------------------------------------------------------------
// Memory operations: 46.01%
//   - Load instructions: 27.59%
//   - Store instructions: 18.42%
// Branch instructions: 9.39%
// NOP instructions: .07%
// Integer division operations: 0%
// Floating-point operations: 0%
// Other instructions (likely integer): 44.51%
// -------------------------------------------------------------------------
// CPI: 0.302
// ====================== END OF ANALYSIS ======================
#include <sys/queue.h>
#include <sys/stat.h>
#include <pthread.h>
#include <semaphore.h>

#include <rte_common.h>
#include <rte_byteorder.h>
#include <rte_log.h>
#include <rte_debug.h>
#include <rte_cycles.h>
#include <rte_memory.h>
#include <rte_memcpy.h>
#include <rte_launch.h>
#include <rte_eal.h>
#include <rte_per_lcore.h>
#include <rte_lcore.h>
#include <rte_atomic.h>
#include <rte_branch_prediction.h>
#include <rte_mempool.h>
#include <rte_mbuf.h>
#include <rte_malloc.h>
#include <rte_interrupts.h>
#include <rte_pci.h>
#include <rte_ether.h>
#include <rte_ethdev.h>
#include <rte_ip.h>
#include <rte_string_fns.h>
#include <rte_flow.h>
#include <stdio.h>
#include <stdlib.h>

#include "testpmd.h"
#if defined(RTE_ARCH_X86)
#include <rte_cpuflags.h>
#include "macswap_sse.h"
#elif defined(__ARM_NEON)
#include "macswap_neon.h"
#else
#include "macswap.h"
#endif

#define NUM_CORE 8

//////////////////////////////////////////////////////////////////////////////////////
__attribute__((always_inline)) inline void dummy_instructions(void) {
    __asm__ volatile (
        ".rept 13\n\t"
        "nop\n\t"
        ".endr\n\t"
        :
        :
        : "memory"
    );
}
__attribute__((always_inline)) static inline void dummy_branches(void) {
    __asm__ volatile (
        ".rept 4\n\t"              // Repeat 3500 times
        "cmp $0, %0\n\t"               // Compare %0 with 0
        "jne 1f\n\t"                   // Jump to label 1 if not equal
        "1:\n\t"                       // Define label 1
        ".endr\n\t"
        :
        : "r"(0)                        // Compare against 0 (always false)
        : "memory", "cc"
    );
}
static volatile int global_dummy = 0;
__attribute__((always_inline)) static inline void dummy_loads(void) {
    __asm__ volatile (
        ".rept 1\n\t"              // Repeat 8000 times (preserves footprint)
        "mov (%0), %%eax\n\t"         // Load from the address in %0 into eax
        ".endr\n\t"
        :
        : "r"(&global_dummy)          // Pass address of global_dummy
        : "eax", "memory"
    );
}
__attribute__((always_inline)) static inline void dummy_stores(void) {
    __asm__ volatile (
        ".rept 1\n\t"             // Repeat 3500 times to maintain footprint.
        "mov $42, (%0)\n\t"          // Store the constant 42 to the address in %0.
        ".endr\n\t"
        :
        : "r"(&global_dummy)
        : "memory"
    );
}

// New helper: heavy_mem_ops_large
// This function processes a moderately sized array many times,
// performing a load–accumulate and a store on each element.
__attribute__((noinline)) void heavy_mem_ops_large(void) {
    // Use a larger array to increase the memory footprint.
    #define MEM_OPS_ARRAY_SIZE 128

    // MEM_OPS_LOOP_COUNT 20 for low, 100 for mid, 1000 for high
    #define MEM_OPS_LOOP_COUNT 1000

    // Allocate a volatile array to force actual memory accesses.
    volatile int arr[MEM_OPS_ARRAY_SIZE] __attribute__((aligned(64)));
    // Initialize the array.
    for (int i = 0; i < MEM_OPS_ARRAY_SIZE; i++) {
        arr[i] = i;
    }

    volatile int sum = 0;
    // Outer loop to increase the number of memory operations.
    for (int j = 0; j < MEM_OPS_LOOP_COUNT; j++) {
        // Inner loop: process each element.
        for (int i = 0; i < MEM_OPS_ARRAY_SIZE; i++) {
            // Load from memory.
            int tmp = arr[i];
            // Accumulate into sum.
            sum += tmp;
            // Store the result back into the array.
            arr[i] = tmp ^ sum;
        }
    }
    // Prevent the compiler from optimizing away the loop.
    if (sum == 0xdeadbeef)
        printf("Impossible!\n");
}

void combined_compute() {

    // dummy_instructions();
    dummy_branches();
    dummy_loads();
    dummy_stores();
    for (int k = 0; k < 1; k++) {
        heavy_mem_ops_large();
    }
}

///////////////////////////////////////////////////////////////////////////////////////



#define MACSWAP_PIPELINE_DEPTH 8
#define MACSWAP_PIPELINE_MASK (MACSWAP_PIPELINE_DEPTH - 1)

_Static_assert((MACSWAP_PIPELINE_DEPTH & MACSWAP_PIPELINE_MASK) == 0,
        "MACSWAP_PIPELINE_DEPTH must be a power of two");

/*
 * A slot remains owned by the forwarding core until it is submitted, by the
 * processing core until it is completed, and by the forwarding core again
 * until it is transmitted and reclaimed.  Keeping the mbuf pointer array in
 * the slot is what makes the handoff asynchronous; a pointer to the packet
 * callback's stack would not survive a real pipeline.
 */
struct macswap_batch {
    uint64_t start_tsc;
    uint16_t nb_rx;
    uint16_t tx_sent;
    struct rte_mbuf *pkts[MAX_PKT_BURST];
} __rte_cache_aligned;

/* Each cursor has a single writer and lives on a separate cache line. */
struct macswap_cursor {
    uint64_t value;
} __rte_cache_aligned;

struct macswap_worker_data;

struct macswap_stream_queue {
    struct macswap_cursor submitted; /* forwarding core -> processing core */
    struct macswap_cursor completed; /* processing core -> forwarding core */
    struct macswap_cursor reclaimed; /* forwarding core; read at shutdown */
    struct macswap_batch *batches;
    struct fwd_stream *fs;
    struct macswap_worker_data *worker;
};

/* Yield shared execution resources to an SMT-sibling processing worker. */
#define MACSWAP_FULL_BACKOFF_PAUSES 32

struct macswap_worker_data {
    struct fwd_lcore *owner;
    struct macswap_stream_queue **queues;
    uint16_t nb_queues;
    unsigned int producer_lcore;
    unsigned int worker_lcore;
    uint32_t abort;
} __rte_cache_aligned;

static struct macswap_worker_data macswap_workers[NUM_CORE]
        __rte_cache_aligned;
static struct macswap_stream_queue **macswap_stream_map;
static size_t macswap_stream_map_size;
static unsigned int macswap_launched_workers;
static bool macswap_pipeline_initialized;
static bool macswap_waitpkg_supported;
static uint64_t macswap_wait_timeout_cycles;

/*
 * Processing-core placement knob.  The first NUM_CORE forwarding lcores are
 * selected by testpmd's --nb-cores option; the lcores below run the MAC-swap
 * stage.  Keep every selected lcore in EAL's -l list.
 */
// unsigned int my_lcore_id[NUM_CORE] = {1, 2, 3, 4, 5, 6, 7, 8}; // Same core w poll mode (not a concurrent pipeline)
// unsigned int my_lcore_id[NUM_CORE] = {10, 11, 12, 13, 14, 15, 16, 17}; // Same subnuma
// unsigned int my_lcore_id[NUM_CORE] = {18, 19, 20, 21, 22, 23, 24, 25}; // Different subnuma
unsigned int my_lcore_id[NUM_CORE] = {36, 37, 38, 39, 40, 41, 42, 43}; // Different numa
// unsigned int my_lcore_id[NUM_CORE] = {73, 74, 75, 76, 77, 78, 79, 80}; // SMT

static inline uint64_t
macswap_load_acquire(const uint64_t *value)
{
    return __atomic_load_n(value, __ATOMIC_ACQUIRE);
}

static inline uint64_t
macswap_load_relaxed(const uint64_t *value)
{
    return __atomic_load_n(value, __ATOMIC_RELAXED);
}

static inline void
macswap_store_release(uint64_t *value, uint64_t new_value)
{
    __atomic_store_n(value, new_value, __ATOMIC_RELEASE);
}

static inline void
macswap_full_backoff(void)
{
    unsigned int pause_count;

    for (pause_count = 0; pause_count < MACSWAP_FULL_BACKOFF_PAUSES;
            pause_count++)
        rte_pause();
}

static inline void
macswap_wait_for_completion(struct macswap_stream_queue *queue,
        uint64_t expected)
{
#if defined(RTE_ARCH_X86)
    if (likely(macswap_waitpkg_supported)) {
        const volatile void *address = &queue->completed.value;

        /* UMONITOR the completion cursor, then close the missed-wakeup race. */
        __asm__ volatile (".byte 0xf3, 0x0f, 0xae, 0xf7"
                : : "D"(address) : "memory");
        if (macswap_load_acquire(&queue->completed.value) == expected) {
            const uint64_t deadline =
                    rte_get_tsc_cycles() + macswap_wait_timeout_cycles;
            const uint32_t deadline_low = (uint32_t)deadline;
            const uint32_t deadline_high = (uint32_t)(deadline >> 32);

            /* Request C0.1: yield execution resources without the C0.2 wake penalty. */
            __asm__ volatile (".byte 0xf2, 0x0f, 0xae, 0xf7"
                    : : "D"(1), "a"(deadline_low), "d"(deadline_high)
                    : "cc", "memory");
        }
        return;
    }
#endif

    macswap_full_backoff();
}

static inline void
macswap_transmit_batch(struct macswap_stream_queue *queue,
        struct macswap_batch *batch)
{
    struct fwd_stream *fs = queue->fs;
    uint16_t nb_tx;
    uint16_t remaining;
    uint16_t sent = 0;
    uint32_t retry;

    remaining = batch->nb_rx - batch->tx_sent;
    nb_tx = rte_eth_tx_burst(fs->tx_port, fs->tx_queue,
            &batch->pkts[batch->tx_sent], remaining);
    batch->tx_sent += nb_tx;
    sent += nb_tx;

    if (unlikely(batch->tx_sent < batch->nb_rx) && fs->retry_enabled) {
        retry = 0;
        while (batch->tx_sent < batch->nb_rx &&
                retry++ < burst_tx_retry_num) {
            rte_delay_us(burst_tx_delay_time);
            remaining = batch->nb_rx - batch->tx_sent;
            nb_tx = rte_eth_tx_burst(fs->tx_port, fs->tx_queue,
                    &batch->pkts[batch->tx_sent], remaining);
            batch->tx_sent += nb_tx;
            sent += nb_tx;
        }
    }

    fs->tx_packets += sent;
    inc_tx_burst_stats(fs, sent);

    if (unlikely(batch->tx_sent != batch->nb_rx)) {
        uint16_t packet;

        fs->fwd_dropped += batch->nb_rx - batch->tx_sent;
        for (packet = batch->tx_sent; packet < batch->nb_rx; packet++)
            rte_pktmbuf_free(batch->pkts[packet]);
        batch->tx_sent = batch->nb_rx;
    }

    /* This is end-to-end pipeline latency when cycle recording is enabled. */
    get_end_cycles(fs, batch->start_tsc);
}

static inline bool
macswap_process_one_queue(struct macswap_stream_queue *queue)
{
    uint64_t completed = macswap_load_relaxed(&queue->completed.value);
    uint64_t submitted = macswap_load_acquire(&queue->submitted.value);
    struct macswap_batch *batch;

    if (completed == submitted)
        return false;

    batch = &queue->batches[completed & MACSWAP_PIPELINE_MASK];
    combined_compute();
    do_macswap(batch->pkts, batch->nb_rx, &ports[queue->fs->tx_port]);

    /* Publish packet/header writes before the forwarding core transmits. */
    macswap_store_release(&queue->completed.value, completed + 1);
    return true;
}

static inline bool
macswap_process_one(struct macswap_worker_data *worker, uint16_t *next_queue)
{
    uint16_t scanned;

    for (scanned = 0; scanned < worker->nb_queues; scanned++) {
        uint16_t queue_id = *next_queue + scanned;

        if (queue_id >= worker->nb_queues)
            queue_id -= worker->nb_queues;

        if (macswap_process_one_queue(worker->queues[queue_id])) {
            *next_queue = queue_id + 1;
            if (*next_queue == worker->nb_queues)
                *next_queue = 0;
            return true;
        }
    }

    return false;
}

static void
macswap_finish_queued_work(struct macswap_worker_data *worker)
{
    uint16_t next_queue = 0;
    uint16_t queue_id;

    while (macswap_process_one(worker, &next_queue))
        ;

    /*
     * The forwarding lcore has stopped, so the worker must transmit any
     * completed slots which the normal completion path did not reclaim.
     */
    for (queue_id = 0; queue_id < worker->nb_queues; queue_id++) {
        struct macswap_stream_queue *queue = worker->queues[queue_id];
        uint64_t reclaimed = macswap_load_acquire(&queue->reclaimed.value);
        uint64_t completed = macswap_load_acquire(&queue->completed.value);

        while (reclaimed != completed) {
            struct macswap_batch *batch =
                    &queue->batches[reclaimed & MACSWAP_PIPELINE_MASK];

            macswap_transmit_batch(queue, batch);
            reclaimed++;
        }
        macswap_store_release(&queue->reclaimed.value, reclaimed);
    }
}

static int
macswap_worker_main(void *arg)
{
    struct macswap_worker_data *worker = arg;
    uint16_t next_queue = 0;

    if (likely(worker->nb_queues == 1)) {
        struct macswap_stream_queue *queue = worker->queues[0];
        struct rte_port *txp = &ports[queue->fs->tx_port];
        uint64_t completed =
                macswap_load_relaxed(&queue->completed.value);

        for (;;) {
            uint64_t submitted =
                    macswap_load_acquire(&queue->submitted.value);
            struct macswap_batch *batch;

            if (completed == submitted) {
                if (unlikely(__atomic_load_n(&worker->abort,
                                __ATOMIC_ACQUIRE) ||
                                worker->owner->stopped))
                    return 0;
                rte_pause();
                continue;
            }

            batch = &queue->batches[completed & MACSWAP_PIPELINE_MASK];
            combined_compute();
            do_macswap(batch->pkts, batch->nb_rx, txp);

            /* Keep the sole-writer cursor local; publish only the new value. */
            completed++;
            macswap_store_release(&queue->completed.value, completed);
        }
    }

    for (;;) {
        if (unlikely(__atomic_load_n(&worker->abort, __ATOMIC_ACQUIRE)))
            return 0;

        if (unlikely(worker->owner->stopped))
            return 0;

        if (!macswap_process_one(worker, &next_queue))
            rte_pause();
    }
}

static inline void
macswap_drain_completions(struct macswap_stream_queue *queue)
{
    uint64_t reclaimed = macswap_load_relaxed(&queue->reclaimed.value);
    uint64_t completed = macswap_load_acquire(&queue->completed.value);

    while (reclaimed != completed) {
        struct macswap_batch *batch =
                &queue->batches[reclaimed & MACSWAP_PIPELINE_MASK];

        macswap_transmit_batch(queue, batch);
        reclaimed++;
    }

    macswap_store_release(&queue->reclaimed.value, reclaimed);
}

static inline struct macswap_stream_queue *
macswap_get_stream_queue(const struct fwd_stream *fs)
{
    size_t index;

    if (unlikely(fs->rx_port >= RTE_MAX_ETHPORTS ||
                    fs->rx_queue >= nb_rxq || macswap_stream_map == NULL))
        return NULL;

    index = (size_t)fs->rx_port * nb_rxq + fs->rx_queue;
    if (unlikely(index >= macswap_stream_map_size))
        return NULL;

    return macswap_stream_map[index];
}

static void
pkt_burst_mac_swap(struct fwd_stream *fs)
{
    struct macswap_stream_queue *queue = macswap_get_stream_queue(fs);
    struct macswap_worker_data *worker;
    struct macswap_batch *batch;
    uint64_t submitted;
    uint64_t reclaimed;
    uint16_t nb_rx;

    if (unlikely(queue == NULL))
        return;

    worker = queue->worker;

    /*
     * A compute-bound helper keeps this queue full for most callbacks.  Check
     * that state before touching the RX/TX path.  No queue memory is freed
     * until both stages have stopped.
     */
    for (;;) {
        submitted = macswap_load_relaxed(&queue->submitted.value);
        reclaimed = macswap_load_relaxed(&queue->reclaimed.value);
        if (likely(submitted - reclaimed != MACSWAP_PIPELINE_DEPTH ||
                macswap_load_acquire(&queue->completed.value) != reclaimed))
            break;

        /*
         * Keep the full-ring wait inside this callback.  Returning to
         * testpmd's forwarding loop just repeats stream lookup and cursor
         * polling on the SMT sibling while Stage B is compute-bound.
         */
        macswap_wait_for_completion(queue, reclaimed);
        if (unlikely(worker->owner->stopped))
            return;
        if (unlikely(worker->nb_queues != 1 &&
                macswap_load_acquire(&queue->completed.value) == reclaimed))
            return;
    }

    submitted = macswap_load_relaxed(&queue->submitted.value);
    reclaimed = macswap_load_relaxed(&queue->reclaimed.value);

    if (unlikely(submitted - reclaimed == MACSWAP_PIPELINE_DEPTH)) {
        macswap_drain_completions(queue);
        reclaimed = macswap_load_relaxed(&queue->reclaimed.value);
        if (submitted - reclaimed == MACSWAP_PIPELINE_DEPTH) {
            macswap_full_backoff();
            return;
        }
    }

    batch = &queue->batches[submitted & MACSWAP_PIPELINE_MASK];
    batch->start_tsc = 0;
    get_start_cycles(&batch->start_tsc);

    nb_rx = rte_eth_rx_burst(fs->rx_port, fs->rx_queue, batch->pkts,
            nb_pkt_per_burst);
    inc_rx_burst_stats(fs, nb_rx);

    if (likely(nb_rx != 0)) {
        fs->rx_packets += nb_rx;
        batch->nb_rx = nb_rx;
        batch->tx_sent = 0;

        /* Publish the fully initialized persistent slot to the worker. */
        macswap_store_release(&queue->submitted.value, submitted + 1);
    }

    /* TX older completed bursts while the worker processes the new one. */
    macswap_drain_completions(queue);
}

static void
macswap_free_unreclaimed(struct macswap_stream_queue *queue)
{
    uint64_t reclaimed = macswap_load_relaxed(&queue->reclaimed.value);
    uint64_t submitted = macswap_load_relaxed(&queue->submitted.value);

    while (reclaimed != submitted) {
        struct macswap_batch *batch =
                &queue->batches[reclaimed & MACSWAP_PIPELINE_MASK];
        uint16_t packet;

        for (packet = batch->tx_sent; packet < batch->nb_rx; packet++)
            rte_pktmbuf_free(batch->pkts[packet]);
        reclaimed++;
    }
}

static void
macswap_pipeline_free(void)
{
    unsigned int worker_id;

    for (worker_id = 0; worker_id < NUM_CORE; worker_id++) {
        struct macswap_worker_data *worker = &macswap_workers[worker_id];
        uint16_t queue_id;

        if (worker->queues != NULL) {
            for (queue_id = 0; queue_id < worker->nb_queues; queue_id++) {
                struct macswap_stream_queue *queue = worker->queues[queue_id];

                if (queue == NULL)
                    continue;
                macswap_free_unreclaimed(queue);
                rte_free(queue->batches);
                rte_free(queue);
            }
        }
        rte_free(worker->queues);
    }

    rte_free(macswap_stream_map);
    macswap_stream_map = NULL;
    macswap_stream_map_size = 0;
    macswap_launched_workers = 0;
    macswap_pipeline_initialized = false;
    macswap_waitpkg_supported = false;
    macswap_wait_timeout_cycles = 0;
    memset(macswap_workers, 0, sizeof(macswap_workers));
}

static bool
macswap_lcore_is_forwarding(unsigned int lcore_id)
{
    unsigned int index;

    for (index = 0; index < cur_fwd_config.nb_fwd_lcores; index++) {
        if (fwd_lcores_cpuids[index] == lcore_id)
            return true;
    }
    return false;
}

static int
macswap_pipeline_begin(__rte_unused portid_t port_id)
{
    unsigned int worker_id;
    int ret = -1;

    if (macswap_pipeline_initialized)
        return 0;

    if (tx_first) {
        fprintf(stderr, "macswap pipeline does not support --tx-first\n");
        return -1;
    }

    if (cur_fwd_config.nb_fwd_lcores == 0 ||
            cur_fwd_config.nb_fwd_lcores > NUM_CORE) {
        fprintf(stderr, "macswap pipeline supports 1 to %u forwarding cores\n",
                NUM_CORE);
        return -1;
    }

#if defined(RTE_ARCH_X86)
    macswap_waitpkg_supported =
            rte_cpu_get_flag_enabled(RTE_CPUFLAG_WAITPKG) > 0;
#endif
    macswap_wait_timeout_cycles = rte_get_tsc_hz();

    macswap_stream_map_size = (size_t)RTE_MAX_ETHPORTS * nb_rxq;
    macswap_stream_map = rte_zmalloc("macswap_stream_map",
            macswap_stream_map_size * sizeof(*macswap_stream_map),
            RTE_CACHE_LINE_SIZE);
    if (macswap_stream_map == NULL)
        goto fail;

    for (worker_id = 0; worker_id < cur_fwd_config.nb_fwd_lcores;
            worker_id++) {
        struct macswap_worker_data *worker = &macswap_workers[worker_id];
        struct fwd_lcore *owner = fwd_lcores[worker_id];
        unsigned int helper_lcore = my_lcore_id[worker_id];
        unsigned int socket_id =
                rte_lcore_to_socket_id(fwd_lcores_cpuids[worker_id]);
        uint16_t queue_id;

        if (!rte_lcore_is_enabled(helper_lcore) ||
                helper_lcore == rte_get_main_lcore() ||
                macswap_lcore_is_forwarding(helper_lcore) ||
                rte_eal_get_lcore_state(helper_lcore) != WAIT) {
            fprintf(stderr, "macswap helper lcore %u is not available\n",
                    helper_lcore);
            goto fail;
        }

        worker->owner = owner;
        worker->producer_lcore = fwd_lcores_cpuids[worker_id];
        worker->worker_lcore = helper_lcore;
        worker->nb_queues = owner->stream_nb;
        owner->stopped = 0;

        printf("macswap pipeline mapping: forwarding lcore %u -> macswap lcore %u\n",
                worker->producer_lcore, worker->worker_lcore);

        worker->queues = rte_zmalloc_socket("macswap_worker_queues",
                worker->nb_queues * sizeof(*worker->queues),
                RTE_CACHE_LINE_SIZE, socket_id);
        if (worker->queues == NULL)
            goto fail;

        for (queue_id = 0; queue_id < worker->nb_queues; queue_id++) {
            struct fwd_stream *fs =
                    fwd_streams[owner->stream_idx + queue_id];
            struct macswap_stream_queue *queue;
            size_t map_index;

            if (fs->rx_port >= RTE_MAX_ETHPORTS || fs->rx_queue >= nb_rxq)
                goto fail;

            map_index = (size_t)fs->rx_port * nb_rxq + fs->rx_queue;
            if (macswap_stream_map[map_index] != NULL) {
                fprintf(stderr,
                        "duplicate macswap pipeline mapping for port %u queue %u\n",
                        fs->rx_port, fs->rx_queue);
                goto fail;
            }

            queue = rte_zmalloc_socket("macswap_stream_queue",
                    sizeof(*queue), RTE_CACHE_LINE_SIZE, socket_id);
            if (queue == NULL)
                goto fail;

            queue->batches = rte_zmalloc_socket("macswap_batch_slots",
                    sizeof(*queue->batches) * MACSWAP_PIPELINE_DEPTH,
                    RTE_CACHE_LINE_SIZE, socket_id);
            if (queue->batches == NULL) {
                rte_free(queue);
                goto fail;
            }

            queue->fs = fs;
            queue->worker = worker;
            worker->queues[queue_id] = queue;
            macswap_stream_map[map_index] = queue;
        }
    }

    for (worker_id = 0; worker_id < cur_fwd_config.nb_fwd_lcores;
            worker_id++) {
        struct macswap_worker_data *worker = &macswap_workers[worker_id];

        ret = rte_eal_remote_launch(macswap_worker_main, worker,
                worker->worker_lcore);
        if (ret != 0) {
            fprintf(stderr, "failed to launch macswap helper on lcore %u: %d\n",
                    worker->worker_lcore, ret);
            goto fail;
        }
        macswap_launched_workers++;
    }

    macswap_pipeline_initialized = true;
    printf("macswap pipeline: %u forwarding cores, %u persistent slots/stream, "
            "full-ring wait=%s\n", cur_fwd_config.nb_fwd_lcores,
            MACSWAP_PIPELINE_DEPTH,
            macswap_waitpkg_supported ? "WAITPKG" : "PAUSE");
    return 0;

fail:
    for (worker_id = 0; worker_id < NUM_CORE; worker_id++) {
        __atomic_store_n(&macswap_workers[worker_id].abort, 1,
                __ATOMIC_RELEASE);
        if (macswap_workers[worker_id].owner != NULL)
            macswap_workers[worker_id].owner->stopped = 1;
    }
    for (worker_id = 0; worker_id < macswap_launched_workers; worker_id++)
        rte_eal_wait_lcore(macswap_workers[worker_id].worker_lcore);
    macswap_pipeline_free();
    return ret;
}

static void
macswap_pipeline_end(__rte_unused portid_t port_id)
{
    unsigned int worker_id;

    if (!macswap_pipeline_initialized)
        return;

    /* Both stages are joined, so shutdown draining is now single-threaded. */
    for (worker_id = 0; worker_id < macswap_launched_workers; worker_id++)
        macswap_finish_queued_work(&macswap_workers[worker_id]);

    macswap_pipeline_free();
}

struct fwd_engine mac_swap_engine = {
    .fwd_mode_name = "macswap",
    .port_fwd_begin = macswap_pipeline_begin,
    .port_fwd_end = macswap_pipeline_end,
    .packet_fwd = pkt_burst_mac_swap,
};
