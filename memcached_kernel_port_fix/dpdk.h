// This file containes the necessary routines to initialize
// and work with DPDK. It's written in C to be able to link with
// any application without issues and any extra steps.
//
// The implementation enables zero-copy networking.

#ifndef _DPDK_H_
#define _DPDK_H_

#include <assert.h>

#include <rte_config.h>
#include <rte_eal.h>
#include <rte_ethdev.h>
#include <rte_ether.h>
#include <rte_mbuf.h>
#include <rte_pdump.h>
#include <rte_malloc.h>

// Global DPDK configs.
static const char *kPacketMemPoolName = "dpdk_packet_mem_pool";

#define kRingN 1
#define kRingDescN 1024
#define kMTUStandardFrames 1500
#define kMTUJumboFrames 9000
#define kLinkTimeOut_ms 1000
#define kMaxBurstSize 128

// To implement new tx pipeline
#define BURST_TX_DRAIN_US 1000000 /* TX drain every ~100us */
#define MAX_RX_QUEUE_PER_LCORE 16
#define MAX_TX_QUEUE_PER_PORT 16

/* Per-port statistics struct */
struct fwd_port_statistics {
	uint64_t tx;
	uint64_t rx;
	uint64_t dropped;
} __rte_cache_aligned;
struct fwd_port_statistics port_statistics[RTE_MAX_ETHPORTS];

// struct lcore_queue_conf {
// 	unsigned n_rx_port;
// 	unsigned rx_port_list[MAX_RX_QUEUE_PER_LCORE];
// } __rte_cache_aligned;

// struct lcore_queue_conf lcore_queue_conf[RTE_MAX_LCORE];
// static uint64_t timer_period = 10; /* default period is 10 seconds */
static struct rte_eth_dev_tx_buffer *tx_buffer[RTE_MAX_ETHPORTS];

// Main DPDK struct.
struct DPDKObj {
  // Main mem pool for this DPDK object.
  struct rte_mempool *mpool;

  // Some port configs and parameters.
  uint16_t pmd_port_cnt;
  uint16_t pmd_ports[RTE_MAX_ETHPORTS];
  struct rte_ether_addr pmd_eth_addrs[RTE_MAX_ETHPORTS];
  uint16_t pmd_port_to_use; // This port will be used throughout this object.

  // Implementing a new transmit pipeline
  // struct lcore_queue_conf *qconf;
  // struct rte_eth_dev_tx_buffer *buffer;
  unsigned lcore_id;

  // TX and RX buffers.
  uint16_t rx_burst_size;
  uint16_t rx_burst_ptr;
  struct rte_mbuf *rx_mbufs[kMaxBurstSize];
  uint16_t tx_burst_size;
  uint16_t tx_burst_ptr;
  struct rte_mbuf *tx_mbufs[kMaxBurstSize];
};

// Initialize DPDK; it returns pmd ports to be used for
// later communication.
static int InitDPDK(struct DPDKObj *dpdk_obj) {
  assert(dpdk_obj != NULL);

  const size_t kDpdkArgcMax = 16;
  int dargv_cnt = 0;
  char *dargv[kDpdkArgcMax];
  dargv[dargv_cnt++] = (char *)"-l";
  dargv[dargv_cnt++] = (char *)"0-3";
  dargv[dargv_cnt++] = (char *)"-n";
  dargv[dargv_cnt++] = (char *)"1";
  dargv[dargv_cnt++] = (char *)"--proc-type";
  dargv[dargv_cnt++] = (char *)"auto";

  int ret = rte_eal_init(dargv_cnt, dargv);
  if (ret < 0) {
    fprintf(stderr, "Failed to initialize DPDK.\n");
    return -1;
  }
  fprintf(stderr, "EAL is initialized!\n");

  ret = rte_pdump_init();
  if (ret) {
    fprintf(stderr, "Failed to initialize pdump.\n");
    return -1;
  }
  fprintf(stderr, "pdump initialized.\n");

  /* Enumerate valid NIC ports */
  int p_num = rte_eth_dev_count_avail();
  if (p_num == 0) {
    fprintf(stderr, "No suitable NICs found; check driver binding and DPDK linking options\n");
    return -1;
  }
  fprintf(stderr, "Available ports: %d\n", p_num);

  dpdk_obj->pmd_port_cnt = 0;
  for (uint16_t i = 0; i < RTE_MAX_ETHPORTS; ++i) {
    if (rte_eth_dev_is_valid_port(i)) {
      dpdk_obj->pmd_ports[dpdk_obj->pmd_port_cnt] = i;
      rte_eth_macaddr_get(i, &dpdk_obj->pmd_eth_addrs[dpdk_obj->pmd_port_cnt]);
      fprintf(stderr, "Found port %u with MAC: ", i);
      for (int j = 0; j < RTE_ETHER_ADDR_LEN; ++j)
        fprintf(stderr, "%02X:", dpdk_obj->pmd_eth_addrs[dpdk_obj->pmd_port_cnt].addr_bytes[j]);
      fprintf(stderr, "\n");
      dpdk_obj->pmd_port_cnt++;
    }
  }

  if (dpdk_obj->pmd_port_cnt == 0) {
    fprintf(stderr, "No valid ports found.\n");
    return -1;
  }

  /* Create the mempool for packets */
  dpdk_obj->mpool = rte_pktmbuf_pool_create(
      kPacketMemPoolName, kRingN * kRingDescN * 2, 0, 0,
      kMTUStandardFrames + RTE_PKTMBUF_HEADROOM, SOCKET_ID_ANY);
  if (dpdk_obj->mpool == NULL) {
    fprintf(stderr, "Failed to create memory pool for packets.\n");
    return -1;
  }

  /* Minimal Ethernet port configuration */
  struct rte_eth_conf port_conf;
  memset(&port_conf, 0, sizeof(port_conf));
  port_conf.link_speeds = ETH_LINK_SPEED_AUTONEG;
  port_conf.rxmode.mq_mode = ETH_MQ_RX_NONE;
  port_conf.rxmode.max_rx_pkt_len = kMTUStandardFrames;
  port_conf.rxmode.offloads = 0;
  port_conf.txmode.mq_mode = ETH_MQ_TX_NONE;
  port_conf.txmode.offloads = 0;

  /* Variables for queue setup */
  uint16_t rx_ring_desc = kRingDescN;
  uint16_t tx_ring_desc = kRingDescN;
  struct rte_eth_dev_info dev_info;

  /* Iterate over each valid port and try to initialize it */
  int chosen_idx = -1;
  for (uint16_t idx = 0; idx < dpdk_obj->pmd_port_cnt; idx++) {
    uint16_t port_id = dpdk_obj->pmd_ports[idx];

    ret = rte_eth_dev_info_get(port_id, &dev_info);
    if (ret) {
      fprintf(stderr, "Failed to get device info for port %u\n", port_id);
      continue;
    }

    ret = rte_eth_dev_configure(port_id, kRingN, kRingN, &port_conf);
    if (ret) {
      fprintf(stderr, "Failed to configure port %u\n", port_id);
      continue;
    }

    ret = rte_eth_dev_set_mtu(port_id, kMTUStandardFrames);
    if (ret) {
      fprintf(stderr, "Failed to set MTU for port %u\n", port_id);
      continue;
    }

    uint16_t rx_desc = rx_ring_desc, tx_desc = tx_ring_desc;
    ret = rte_eth_dev_adjust_nb_rx_tx_desc(port_id, &rx_desc, &tx_desc);
    if (ret) {
      fprintf(stderr, "Failed to adjust descriptors for port %u\n", port_id);
      continue;
    }

    ret = rte_eth_tx_queue_setup(port_id, 0, tx_desc, SOCKET_ID_ANY, &dev_info.default_txconf);
    if (ret) {
      fprintf(stderr, "Failed to setup TX queue for port %u\n", port_id);
      continue;
    }

    ret = rte_eth_rx_queue_setup(port_id, 0, rx_desc, SOCKET_ID_ANY, &dev_info.default_rxconf, dpdk_obj->mpool);
    if (ret) {
      fprintf(stderr, "Failed to setup RX queue for port %u\n", port_id);
      continue;
    }

    /* Initialize TX buffer for this port */
    tx_buffer[port_id] = (struct rte_eth_dev_tx_buffer *)
        rte_zmalloc_socket("tx_buffer", RTE_ETH_TX_BUFFER_SIZE(kMaxBurstSize),
                           0, rte_eth_dev_socket_id(port_id));
    if (tx_buffer[port_id] == NULL)
      rte_exit(EXIT_FAILURE, "Cannot allocate tx buffer for port %u\n", port_id);

    rte_eth_tx_buffer_init(tx_buffer[port_id], kMaxBurstSize);
    ret = rte_eth_tx_buffer_set_err_callback(tx_buffer[port_id],
                rte_eth_tx_buffer_count_callback, &port_statistics[port_id].dropped);
    if (ret < 0) {
      fprintf(stderr, "Cannot set error callback for port %u\n", port_id);
      continue;
    }

    /* Start the port */
    ret = rte_eth_dev_start(port_id);
    if (ret) {
      fprintf(stderr, "Failed to start port %u\n", port_id);
      continue;
    }

    // (Optional) Enable promiscuous mode if desired:
    // rte_eth_promiscuous_enable(port_id);

    /* Wait for link to come up */
    struct rte_eth_link link;
    memset(&link, 0, sizeof(link));
    size_t tout = 0;
    while (tout < kLinkTimeOut_ms && link.link_status == ETH_LINK_DOWN) {
      rte_eth_link_get_nowait(port_id, &link);
      tout++;
      usleep(1000);
    }

    if (link.link_status == ETH_LINK_UP) {
      fprintf(stderr, "Port %u link is UP: speed=%u Mbps, duplex=%d\n",
              port_id, link.link_speed, link.link_duplex);
      chosen_idx = idx;
      break;  // Found a connected port; stop searching.
    } else {
      fprintf(stderr, "Port %u link is DOWN, skipping\n", port_id);
      rte_eth_dev_stop(port_id);
    }
  }

  if (chosen_idx == -1) {
    fprintf(stderr, "No connected port found.\n");
    return -1;
  }

  /* Use the chosen port */
  dpdk_obj->pmd_port_to_use = chosen_idx;
  uint16_t chosen_port = dpdk_obj->pmd_ports[chosen_idx];

  /* Print final port capabilities */
  ret = rte_eth_dev_info_get(chosen_port, &dev_info);
  if (ret < 0) {
    fprintf(stderr, "Failed to get device info for chosen port %u: %s\n",
            chosen_port, rte_strerror(-ret));
    return -1;
  }
  fprintf(stderr, "Port capabilities: %s%s%s%s\n",
          (dev_info.rx_offload_capa & DEV_RX_OFFLOAD_CHECKSUM) ? "RX_CHECKSUM " : "",
          (dev_info.tx_offload_capa & DEV_TX_OFFLOAD_IPV4_CKSUM) ? "TX_IPV4_CHECKSUM " : "",
          (dev_info.tx_offload_capa & DEV_TX_OFFLOAD_TCP_TSO) ? "TCP_TSO " : "",
          (dev_info.tx_offload_capa & DEV_TX_OFFLOAD_UDP_TSO) ? "UDP_TSO " : "");

  return 0;
}

static void FreeDPDKPacket(struct rte_mbuf *packet) {
  rte_pktmbuf_free(packet);
}

// Allocate DPDK buffers for the transmission of batch_size packets.
static int AllocateDPDKTxBuffers(struct DPDKObj *dpdk_obj, size_t batch_size) {
  if (batch_size > kMaxBurstSize)
    return -1;

  int ret =
      rte_pktmbuf_alloc_bulk(dpdk_obj->mpool, dpdk_obj->tx_mbufs, batch_size);
  if (ret)
    return -1;
  dpdk_obj->tx_burst_size = batch_size;
  dpdk_obj->tx_burst_ptr = 0;

  return 0;
}

// Get pointer to the next tx buffer from the pre-allocated burst pool.
static struct rte_mbuf *GetNextDPDKTxBuffer(struct DPDKObj *dpdk_obj) {
  if (dpdk_obj->tx_burst_ptr >= dpdk_obj->tx_burst_size)
    return NULL;
  struct rte_mbuf *mbuf = dpdk_obj->tx_mbufs[dpdk_obj->tx_burst_ptr];
  ++dpdk_obj->tx_burst_ptr;
  return mbuf;
}

// Get pointer to the payload.
static struct rte_mbuf *GetNextDPDKRxBuffer(struct DPDKObj *dpdk_obj) {
  if (dpdk_obj->rx_burst_ptr >= dpdk_obj->rx_burst_size)
    return NULL;
  struct rte_mbuf *mbuf = dpdk_obj->rx_mbufs[dpdk_obj->rx_burst_ptr];
  ++dpdk_obj->rx_burst_ptr;
  return mbuf;
}

static uint8_t *ExtractPacketPayload(struct rte_mbuf *pckt) {
  return rte_pktmbuf_mtod(pckt, uint8_t *) + sizeof(struct rte_ether_hdr);
}

static void AppendPacketHeader(struct DPDKObj *dpdk_obj, struct rte_mbuf *pckt,
                               const struct rte_ether_addr *dst_mac,
                               size_t length) {
  // Packet header.
  size_t pkt_size = sizeof(struct rte_ether_hdr) + length;
  pckt->data_len = pkt_size;
  pckt->pkt_len = pkt_size;
  // fprintf(stderr, "After appending packet header, pkt_size is %lu\n", pkt_size);

  // Ethernet header.
  struct rte_ether_hdr *eth_hdr =
      rte_pktmbuf_mtod(pckt, struct rte_ether_hdr *);
  rte_ether_addr_copy(&dpdk_obj->pmd_eth_addrs[dpdk_obj->pmd_port_to_use],
                      &eth_hdr->s_addr);
  rte_ether_addr_copy(dst_mac, &eth_hdr->d_addr);
  // We use will RTE_ETHER_TYPE_IPV4 header type to avoid any issues on the
  // switch, but we won't actually use IP.
  eth_hdr->ether_type = rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV4);
}

// Send a batch of packets sitting so far in tx_mbuf's.
static int SendBatch(struct DPDKObj *dpdk_obj) {
  // Send packet.
  const uint16_t burst_size = dpdk_obj->tx_burst_size;
  const uint16_t ring_id = 0;
  uint16_t pckt_sent =
      rte_eth_tx_burst(dpdk_obj->pmd_ports[dpdk_obj->pmd_port_to_use], ring_id,
                       dpdk_obj->tx_mbufs, burst_size);
  if (pckt_sent != burst_size) {
    fprintf(stderr, "Failed to send all %d packets, only %d was sent.\n",
            burst_size, pckt_sent);
    rte_pktmbuf_free_bulk(dpdk_obj->tx_mbufs + pckt_sent,
                          (unsigned int)(burst_size - pckt_sent));
    return -1;
  }
  // rte_pktmbuf_free_bulk(dpdk_obj->tx_mbufs, burst_size);
  dpdk_obj->tx_burst_ptr = 0;
  return 0;
}

// Send a batch of packets sitting so far in tx_mbuf's.
static int SendBuffer(struct DPDKObj *dpdk_obj, struct rte_mbuf *pckt) {
  // Send packet.
  uint16_t pckt_sent;
  const uint16_t ring_id = 0;
  struct rte_eth_dev_tx_buffer *buffer;
  const uint16_t burst_size = dpdk_obj->tx_burst_size;

  buffer = tx_buffer[dpdk_obj->pmd_ports[dpdk_obj->pmd_port_to_use]];
  pckt_sent = rte_eth_tx_buffer(dpdk_obj->pmd_ports[dpdk_obj->pmd_port_to_use],ring_id,buffer,pckt);
  
  if(!pckt_sent)
    return 0;
  else {
    if(pckt_sent == kMaxBurstSize) {
      //fprintf(stderr, "DPDK-Version of Memcached Server is sending out %d Packets (from rte_eth_tx_buffer() API) in a single Burst!\n", (int) pckt_sent);
      //rte_pktmbuf_free(dpdk_obj->tx_mbufs[i]);
      dpdk_obj->tx_burst_ptr = 0;
      return 0;
    }
    else{
      //fprintf(stderr, "Failed to send all %d packets (from rte_eth_tx_buffer() API), only %d was sent.\n",
      //      burst_size, pckt_sent);
      // rte_pktmbuf_free_bulk(dpdk_obj->tx_mbufs, pckt_sent); //We explicitly free sent pckts -> rte_eth_tx_buffer will free unsent pckts
      return -1;
    }
  }
}

// Receive one or many packets and store them in pckts.
// Returns the number of packets received.
// This is a non-blocking call.
static int RecvOverDPDK(struct DPDKObj *dpdk_obj) {
  struct rte_mbuf *packets[kMaxBurstSize];
  const uint16_t ring_id = 0;
  uint16_t received_pckt_cnt =
      rte_eth_rx_burst(dpdk_obj->pmd_ports[dpdk_obj->pmd_port_to_use], ring_id,
                       packets, kMaxBurstSize);
  if (received_pckt_cnt == 0)
    return 0;

  dpdk_obj->rx_burst_size = 0;
  for (int i = 0; i < received_pckt_cnt; ++i) {
    struct rte_ether_hdr *eth_hdr =
        rte_pktmbuf_mtod(packets[i], struct rte_ether_hdr *);
    // Skip not our packets.
    if (rte_be_to_cpu_16(eth_hdr->ether_type) != RTE_ETHER_TYPE_IPV4) {
      FreeDPDKPacket(packets[i]);
      continue;
    }

    // Store the payload pointers.
    *(dpdk_obj->rx_mbufs + dpdk_obj->rx_burst_size) = packets[i];
    ++dpdk_obj->rx_burst_size;
  }

  dpdk_obj->rx_burst_ptr = 0;
  return dpdk_obj->rx_burst_size;
}

#endif
