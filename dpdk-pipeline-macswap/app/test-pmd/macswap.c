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
#include <pthread.h>

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

#define _GNU_SOURCE

static pthread_t macswap_thread;
static volatile int macswap_thread_running = 0;
static pthread_mutex_t macswap_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t macswap_cond = PTHREAD_COND_INITIALIZER;
static struct fwd_stream *current_fs = NULL;

void set_affinity(int core_id) {
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(core_id, &cpuset);
    pthread_t current_thread = pthread_self();
    if (pthread_setaffinity_np(current_thread, sizeof(cpu_set_t), &cpuset) != 0) {
        perror("pthread_setaffinity_np");
        exit(EXIT_FAILURE);
    }
}

static void *macswap_thread_func(void *arg) {
    set_affinity(10);  // Set affinity to core 10

    while (macswap_thread_running) {
        pthread_mutex_lock(&macswap_mutex);
        while (current_fs == NULL && macswap_thread_running) {
            pthread_cond_wait(&macswap_cond, &macswap_mutex);
        }

        if (!macswap_thread_running) {
            pthread_mutex_unlock(&macswap_mutex);
            break;
        }

        struct fwd_stream *fs = current_fs;
        current_fs = NULL;
        pthread_mutex_unlock(&macswap_mutex);

        // Perform the macswap operation
        struct rte_mbuf  *pkts_burst[MAX_PKT_BURST];
        struct rte_port  *txp;
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

    return NULL;
}

static void pkt_burst_mac_swap(struct fwd_stream *fs) {
    pthread_mutex_lock(&macswap_mutex);
    current_fs = fs;
    pthread_cond_signal(&macswap_cond);
    pthread_mutex_unlock(&macswap_mutex);
}

static void mac_swap_start(void) {
    macswap_thread_running = 1;
    if (pthread_create(&macswap_thread, NULL, macswap_thread_func, NULL) != 0) {
        rte_exit(EXIT_FAILURE, "Error creating macswap thread\n");
    }
}

static void mac_swap_stop(void) {
    macswap_thread_running = 0;
    pthread_mutex_lock(&macswap_mutex);
    pthread_cond_signal(&macswap_cond);
    pthread_mutex_unlock(&macswap_mutex);
    pthread_join(macswap_thread, NULL);
}

struct fwd_engine mac_swap_engine = {
    .fwd_mode_name  = "macswap",
    .port_fwd_begin = mac_swap_start,
    .port_fwd_end   = mac_swap_stop,
    .packet_fwd     = pkt_burst_mac_swap,
};