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
#include <rte_interrupts.h>
#include <rte_pci.h>
#include <rte_ether.h>
#include <rte_ethdev.h>
#include <rte_ip.h>
#include <rte_string_fns.h>
#include <rte_flow.h>

#include "testpmd.h"
#if defined(RTE_ARCH_X86)
#include "macswap_sse.h"
#elif defined(__ARM_NEON)
#include "macswap_neon.h"
#else
#include "macswap.h"
#endif

struct macswap_thread_data {
    struct rte_mbuf **pkts_burst;
    uint16_t nb_rx;
    struct rte_port *txp;
    volatile int ready;
    uint16_t rx_queue;  // RX queue assigned to this core
    uint16_t tx_queue;  // TX queue assigned to this core
};

static int macswap_thread_func(void *arg)
{
    struct macswap_thread_data *data = (struct macswap_thread_data *)arg;


    while (1) {
        if (__sync_bool_compare_and_swap(&data->ready, 1, 0)) {
            // printf("DEBUG: Processing %d packets\n", data->nb_rx);
            do_macswap(data->pkts_burst, data->nb_rx, data->txp);
        }
        rte_pause();  // Yield CPU to prevent busy-waiting
    }

    return 0;
}

extern int my_lcore_id[2] = {10, 11};

static void pkt_burst_mac_swap(struct fwd_stream *fs)
{

    static struct macswap_thread_data macswap_data[2] = {0};
    static int thread_initialized[2] = {0};
    struct rte_mbuf *pkts_burst[MAX_PKT_BURST];
    struct rte_port *txp;
    uint16_t nb_rx;
    uint16_t nb_tx;
    uint32_t retry;
    uint64_t start_tsc = 0;

    int core_index = rte_lcore_index(rte_lcore_id()) - 1;  // Adjust index based on your core mapping


    if (core_index < 0 || core_index >= 2) {
        rte_exit(EXIT_FAILURE, "Invalid core index: %d\n", core_index);
    }

    if (!thread_initialized[core_index]) {

        // Assign RXQ and TXQ to this core
        macswap_data[core_index].rx_queue = core_index;  // RXQ 0 for core 10, RXQ 1 for core 11
        macswap_data[core_index].tx_queue = core_index;  // TXQ 0 for core 10, TXQ 1 for core 11

        int ret = rte_eal_remote_launch(macswap_thread_func, &macswap_data[core_index], my_lcore_id[core_index]);
        if (ret < 0) {
            rte_exit(EXIT_FAILURE, "Failed to launch macswap_thread_func on core %d\n", my_lcore_id[core_index]);
        }
        thread_initialized[core_index] = 1;
    }

    // Only process packets for the assigned queue
    if (fs->rx_queue != macswap_data[core_index].rx_queue || fs->tx_queue != macswap_data[core_index].tx_queue) {
        return;
    }

    get_start_cycles(&start_tsc);

    nb_rx = rte_eth_rx_burst(fs->rx_port, fs->rx_queue, pkts_burst, nb_pkt_per_burst);
    inc_rx_burst_stats(fs, nb_rx);
    if (unlikely(nb_rx == 0)) {
        return;
    }


    fs->rx_packets += nb_rx;
    txp = &ports[fs->tx_port];

    // Protect shared data with mutex
    macswap_data[core_index].pkts_burst = pkts_burst;
    macswap_data[core_index].nb_rx = nb_rx;
    macswap_data[core_index].txp = txp;
    macswap_data[core_index].ready = 1;

    nb_tx = rte_eth_tx_burst(fs->tx_port, fs->tx_queue, pkts_burst, nb_rx);

    if (unlikely(nb_tx < nb_rx) && fs->retry_enabled) {
        retry = 0;
        while (nb_tx < nb_rx && retry++ < burst_tx_retry_num) {
            rte_delay_us(burst_tx_delay_time);
            nb_tx += rte_eth_tx_burst(fs->tx_port, fs->tx_queue, &pkts_burst[nb_tx], nb_rx - nb_tx);
        }
    }

    fs->tx_packets += nb_tx;
    inc_tx_burst_stats(fs, nb_tx);
    if (unlikely(nb_tx < nb_rx)) {
        fs->fwd_dropped += (nb_rx - nb_tx);
        do {
            rte_pktmbuf_free(pkts_burst[nb_tx]);
        } while (++nb_tx < nb_rx);
    }

    get_end_cycles(fs, start_tsc);
}

struct fwd_engine mac_swap_engine = {
    .fwd_mode_name = "macswap",
    .port_fwd_begin = NULL,
    .port_fwd_end = NULL,
    .packet_fwd = pkt_burst_mac_swap,
};