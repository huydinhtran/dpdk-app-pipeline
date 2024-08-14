/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright 2014-2020 Mellanox Technologies, Ltd
 */

#include <stdarg.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <stdint.h>
#include <unistd.h>
#include <inttypes.h>

#include <sys/queue.h>
#include <sys/stat.h>

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

static volatile int macswap_thread_running = 0;

// Function to be executed on the DPDK lcore
static int macswap_lcore_func(void *arg)
{
    struct fwd_stream *fs = (struct fwd_stream *)arg;
    
    while (macswap_thread_running) {
        struct rte_mbuf *pkts_burst[MAX_PKT_BURST];
        struct rte_port *txp;
        uint16_t nb_rx;
        uint16_t nb_tx;
        uint32_t retry;
        uint64_t start_tsc = 0;

        get_start_cycles(&start_tsc);

        nb_rx = rte_eth_rx_burst(fs->rx_port, fs->rx_queue, pkts_burst, nb_pkt_per_burst);
        inc_rx_burst_stats(fs, nb_rx);
        if (likely(nb_rx != 0)) {
            fs->rx_packets += nb_rx;
            txp = &ports[fs->tx_port];

            do_macswap(pkts_burst, nb_rx, txp);

            nb_tx = rte_eth_tx_burst(fs->tx_port, fs->tx_queue, pkts_burst, nb_rx);
            if (unlikely(nb_tx < nb_rx) && fs->retry_enabled) {
                retry = 0;
                while (nb_tx < nb_rx && retry++ < burst_tx_retry_num) {
                    rte_delay_us(burst_tx_delay_time);
                    nb_tx += rte_eth_tx_burst(fs->tx_port, fs->tx_queue,
                            &pkts_burst[nb_tx], nb_rx - nb_tx);
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
        }
        get_end_cycles(fs, start_tsc);
    }

    return 0;
}

static void pkt_burst_mac_swap(struct fwd_stream *fs)
{
    if (!macswap_thread_running) {
        macswap_thread_running = 1;
        // unsigned lcore_id = rte_get_next_lcore(rte_lcore_id(), 1, 0);
        unsigned lcore_id = 2;
        // printf("macswap thread running on lcore %u\n", lcore_id);
        if (lcore_id == RTE_MAX_LCORE) {
            rte_exit(EXIT_FAILURE, "No available lcore for macswap function\n");
        }
        if (rte_eal_remote_launch(macswap_lcore_func, fs, lcore_id) != 0) {
            rte_exit(EXIT_FAILURE, "Error launching macswap function on lcore %u\n", lcore_id);
        }
    }
}

static void mac_swap_start(void)
{
    macswap_thread_running = 0;  // Will be set to 1 in pkt_burst_mac_swap
}

static void mac_swap_stop(void)
{
    macswap_thread_running = 0;
    rte_eal_mp_wait_lcore();
}

struct fwd_engine mac_swap_engine = {
    .fwd_mode_name  = "macswap",
    .port_fwd_begin = mac_swap_start,
    .port_fwd_end   = mac_swap_stop,
    .packet_fwd     = pkt_burst_mac_swap,
};
