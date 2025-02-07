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

#define NUM_CORE 8

struct macswap_thread_data {
    struct rte_mbuf **pkts_burst;
    uint16_t nb_rx;
    struct rte_port *txp;
    rte_atomic32_t ready;  // Use DPDK atomic instead of pthread mutex
    volatile bool stop;
    int core_id;
};

static int macswap_thread_func(void *arg)
{
    struct macswap_thread_data *data = (struct macswap_thread_data *)arg;
    
    while (!data->stop) {
        // Check if work is available using atomic operation
        if (rte_atomic32_read(&data->ready)) {
            // Process packets
            if (data->nb_rx > 0) {
                do_macswap(data->pkts_burst, data->nb_rx, data->txp);
            }
            
            // Mark work as complete
            rte_atomic32_set(&data->ready, 0);
        }
        
        // Use DPDK's optimized pause instead of pthread condition wait
        rte_pause();
    }
    
    return 0;
}

static void init_thread_data(struct macswap_thread_data *data, int core_id)
{
    memset(data, 0, sizeof(struct macswap_thread_data));
    rte_atomic32_init(&data->ready);
    data->core_id = core_id;
    data->stop = false;
}

int my_lcore_id[NUM_CORE] = {1, 2, 3, 4, 5, 6, 7, 8}; // Same core w poll mode
// int my_lcore_id[NUM_CORE] = {10, 11, 12, 13, 14, 15, 16, 17}; // Same subnuma
// int my_lcore_id[NUM_CORE] = {18, 19, 20, 21, 22, 23, 24, 25}; // Different subnuma
// int my_lcore_id[NUM_CORE] = {36, 37, 38, 39, 40, 41, 42, 43}; // Different numa
// int my_lcore_id[NUM_CORE] = {73, 74, 75, 76, 77, 78, 79, 80}; // SMT


static void pkt_burst_mac_swap(struct fwd_stream *fs)
{
    static struct macswap_thread_data macswap_data[NUM_CORE] = {0};
    static int thread_initialized[NUM_CORE] = {0};
    struct rte_mbuf *pkts_burst[MAX_PKT_BURST];
    struct rte_port *txp;
    uint16_t nb_rx;
    uint16_t nb_tx;
    uint32_t retry;
    uint64_t start_tsc = 0;

    // Calculate core index based on queue number
    int core_index = fs->rx_queue % NUM_CORE;
    
    // Initialize thread if needed
    if (!thread_initialized[core_index]) {
        printf("DEBUG: Initializing thread on core %d\n", my_lcore_id[core_index]);
        init_thread_data(&macswap_data[core_index], my_lcore_id[core_index]);
        
        int ret = rte_eal_remote_launch(macswap_thread_func, 
                                      &macswap_data[core_index], 
                                      my_lcore_id[core_index]);
        if (ret < 0) {
            rte_exit(EXIT_FAILURE, "Failed to launch macswap_thread_func on core %d\n", 
                    my_lcore_id[core_index]);
        }
        thread_initialized[core_index] = 1;
    }

    get_start_cycles(&start_tsc);

    // Receive packets
    nb_rx = rte_eth_rx_burst(fs->rx_port, fs->rx_queue, pkts_burst, nb_pkt_per_burst);
    inc_rx_burst_stats(fs, nb_rx);
    if (unlikely(nb_rx == 0))
        return;

    fs->rx_packets += nb_rx;
    txp = &ports[fs->tx_port];

    // Pass data to worker thread using atomic operations
    while (rte_atomic32_read(&macswap_data[core_index].ready)) {
        rte_pause();  // Wait for previous work to complete
    }
    
    macswap_data[core_index].pkts_burst = pkts_burst;
    macswap_data[core_index].nb_rx = nb_rx;
    macswap_data[core_index].txp = txp;
    
    // Signal work is ready
    rte_atomic32_set(&macswap_data[core_index].ready, 1);

    // Wait for processing completion
    while (rte_atomic32_read(&macswap_data[core_index].ready)) {
        rte_pause();
    }

    // Transmit processed packets
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
    
    get_end_cycles(fs, start_tsc);
}
struct fwd_engine mac_swap_engine = {
    .fwd_mode_name = "macswap",
    .port_fwd_begin = NULL,
    .port_fwd_end = NULL,
    .packet_fwd = pkt_burst_mac_swap,
};


/////////////////////////////////////VANILLA//////////////////////////////////////////
// /* SPDX-License-Identifier: BSD-3-Clause
//  * Copyright 2014-2020 Mellanox Technologies, Ltd
//  */

//  #include <stdarg.h>
//  #include <string.h>
//  #include <stdio.h>
//  #include <errno.h>
//  #include <stdint.h>
//  #include <unistd.h>
//  #include <inttypes.h>
 
//  #include <sys/queue.h>
//  #include <sys/stat.h>
 
//  #include <rte_common.h>
//  #include <rte_byteorder.h>
//  #include <rte_log.h>
//  #include <rte_debug.h>
//  #include <rte_cycles.h>
//  #include <rte_memory.h>
//  #include <rte_memcpy.h>
//  #include <rte_launch.h>
//  #include <rte_eal.h>
//  #include <rte_per_lcore.h>
//  #include <rte_lcore.h>
//  #include <rte_atomic.h>
//  #include <rte_branch_prediction.h>
//  #include <rte_mempool.h>
//  #include <rte_mbuf.h>
//  #include <rte_interrupts.h>
//  #include <rte_pci.h>
//  #include <rte_ether.h>
//  #include <rte_ethdev.h>
//  #include <rte_ip.h>
//  #include <rte_string_fns.h>
//  #include <rte_flow.h>
 
//  #include "testpmd.h"
//  #if defined(RTE_ARCH_X86)
//  #include "macswap_sse.h"
//  #elif defined(__ARM_NEON)
//  #include "macswap_neon.h"
//  #else
//  #include "macswap.h"
//  #endif
 
//  /*
//   * MAC swap forwarding mode: Swap the source and the destination Ethernet
//   * addresses of packets before forwarding them.
//   */
//  static void
//  pkt_burst_mac_swap(struct fwd_stream *fs)
//  {
//      struct rte_mbuf  *pkts_burst[MAX_PKT_BURST];
//      struct rte_port  *txp;
//      uint16_t nb_rx;
//      uint16_t nb_tx;
//      uint32_t retry;
//      uint64_t start_tsc = 0;
 
//      get_start_cycles(&start_tsc);
 
//      /*
//       * Receive a burst of packets and forward them.
//       */
//      nb_rx = rte_eth_rx_burst(fs->rx_port, fs->rx_queue, pkts_burst,
//                   nb_pkt_per_burst);
//      inc_rx_burst_stats(fs, nb_rx);
//      if (unlikely(nb_rx == 0))
//          return;
 
//      fs->rx_packets += nb_rx;
//      txp = &ports[fs->tx_port];
 
//      do_macswap(pkts_burst, nb_rx, txp);
 
//      nb_tx = rte_eth_tx_burst(fs->tx_port, fs->tx_queue, pkts_burst, nb_rx);
//      /*
//       * Retry if necessary
//       */
//      if (unlikely(nb_tx < nb_rx) && fs->retry_enabled) {
//          retry = 0;
//          while (nb_tx < nb_rx && retry++ < burst_tx_retry_num) {
//              rte_delay_us(burst_tx_delay_time);
//              nb_tx += rte_eth_tx_burst(fs->tx_port, fs->tx_queue,
//                      &pkts_burst[nb_tx], nb_rx - nb_tx);
//          }
//      }
//      fs->tx_packets += nb_tx;
//      inc_tx_burst_stats(fs, nb_tx);
//      if (unlikely(nb_tx < nb_rx)) {
//          fs->fwd_dropped += (nb_rx - nb_tx);
//          do {
//              rte_pktmbuf_free(pkts_burst[nb_tx]);
//          } while (++nb_tx < nb_rx);
//      }
//      get_end_cycles(fs, start_tsc);
//  }
 
//  struct fwd_engine mac_swap_engine = {
//      .fwd_mode_name  = "macswap",
//      .port_fwd_begin = NULL,
//      .port_fwd_end   = NULL,
//      .packet_fwd     = pkt_burst_mac_swap,
//  };