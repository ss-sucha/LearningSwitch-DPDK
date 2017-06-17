/*-
 *   BSD LICENSE
 *
 *   Copyright(c) 2017, Sucha Supittayapornpong
 *   All rights reserved.
 *
 *   Redistribution and use in source and binary forms, with or without
 *   modification, are permitted provided that the following conditions
 *   are met:
 *
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in
 *       the documentation and/or other materials provided with the
 *       distribution.
 *     * Neither the name of Intel Corporation nor the names of its
 *       contributors may be used to endorse or promote products derived
 *       from this software without specific prior written permission.
 *
 *   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *   "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *   LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 *   A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 *   OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 *   SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 *   LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 *   DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 *   THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 *   (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 *   OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <stdio.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <signal.h>
#include <pthread.h>
#include <getopt.h>
#include <string.h>

#include <rte_eal.h>
#include <rte_mbuf.h>
#include <rte_virtio_net.h>
#include <rte_malloc.h>
#include <rte_log.h>
#include <rte_ethdev.h>
#include <rte_hash.h>

#include "main.h"

#define NUM_MBUFS 8192
#define MBUF_CACHE_SIZE 128
#define MAX_PKT_BURST 32

#define RX_RING_SIZE 128
#define TX_RING_SIZE 512

#define NUM_HASH_ENTRIES 256

static struct rte_mempool *Mbuf_pool;
static int Nb_ports = 0;
static int Nb_sockets = 0;
static char *Socket_files = NULL;

static struct dev_info_tailq Dev_list;
static pthread_mutex_t Dev_list_mutex;
static volatile int Dev_list_update;
static volatile int Force_exit = 0;

static struct rte_hash *Mac_output_map;
static struct dev_info *Output_table[NUM_HASH_ENTRIES];


/*
 * This function displays usage for command line arguments.
 */
static void
print_usage(void)
{
	printf("\nTo add each socket file, use parameter -s or --socket-file\n"
	       "For example:\n"
	       "./learning-switch -c 3 -n 4 -m 256 -- "
	       "-s /PATH/TO/SOCKET/FILE1 "
	       "--socket-file /PATH/TO/SOCKET/FILE2\n\n");
}


/*
 * This function parses arguments and obtains vhost socket files.
 */
static int
parse_socket_paths(int argc, char **argv) {
	char opt;

	static struct option long_option[] = {
		{ "help", no_argument, NULL, 'h'},
		{ "socket-file", required_argument, NULL, 's'},
		{ NULL, 0, 0, 0},
	};

	while((opt = getopt_long(argc, argv, "hs:", long_option, NULL)) != -1) {
		switch (opt) {
		case 'h':
			print_usage();
			exit(0);
			break;
		case 's':
			if(strlen(optarg) > PATH_MAX) {
				RTE_LOG(INFO, LSWITCH_CONFIG,
					"Invalid path length for socket name "
					"(Max %d characters)\n", PATH_MAX);
				return -1;
			}
			Socket_files = realloc(Socket_files,
					       PATH_MAX * (Nb_sockets + 1));
			snprintf(Socket_files + Nb_sockets * PATH_MAX, PATH_MAX,
				 "%s", optarg);
			Nb_sockets++;
			break;
		default:
			RTE_LOG(INFO, LSWITCH_CONFIG, "Invalid arguments\n");
			print_usage();
			return -1;
		}
	}
	return 0;
}


/*
 * This function initializes a physical port.
 */
static int
port_init(int portid)
{
	const struct rte_eth_conf port_conf = {
		.rxmode = { .max_rx_pkt_len = ETHER_MAX_LEN }
	};
	const uint16_t rx_rings = 1;
	const uint16_t tx_rings = 1;
	const uint16_t qid = 0;
	struct dev_info *dev;
	int ret;

	/* Configure the Ethernet device */
	ret = rte_eth_dev_configure(portid, rx_rings, tx_rings, &port_conf);
	if(ret != 0)
		return ret;

	/* Setup RX queue */
	ret = rte_eth_rx_queue_setup(portid, qid, RX_RING_SIZE,
				     rte_eth_dev_socket_id(portid), NULL,
				     Mbuf_pool);
	if(ret < 0)
		return ret;

	/* Setup TX queue */
	ret = rte_eth_tx_queue_setup(portid, qid, TX_RING_SIZE,
				     rte_eth_dev_socket_id(portid), NULL);
	if(ret < 0)
		return ret;

	/* Start the Ethernet port */
	ret = rte_eth_dev_start(portid);
	if(ret < 0)
		return ret;

	/* Display the port MAC address */
	struct ether_addr addr;
	rte_eth_macaddr_get(portid, &addr);
	RTE_LOG(INFO, LSWITCH_CONFIG, "Port %u MAC: "
		"%02" PRIx8 "%02" PRIx8 "%02" PRIx8
		"%02" PRIx8 "%02" PRIx8 "%02" PRIx8 "\n",
		(unsigned)portid,
		addr.addr_bytes[0], addr.addr_bytes[1],
		addr.addr_bytes[2], addr.addr_bytes[3],
		addr.addr_bytes[4], addr.addr_bytes[5]);

	/* Enable RX in promiscuous mode */
	rte_eth_promiscuous_enable(portid);

	/* Store port information in the device list */
	dev = (struct dev_info *)rte_zmalloc("device info", sizeof(*dev),
					     RTE_CACHE_LINE_SIZE);
	if(dev == NULL) {
		return -1;
	}
	dev->virtual = 0;
	dev->id = portid;
	dev->state = DEVICE_READY;
	TAILQ_INSERT_TAIL(&Dev_list, dev, dev_entry);
	RTE_LOG(INFO, LSWITCH_CONFIG,
		"Physical port %d is added to the device list\n", dev->id);

	return 0;
}


/*
 * A callback function when a new virtual device is detected.
 */
static int
new_vdev_callback(int vid)
{
	struct dev_info *dev;

	dev = (struct dev_info *)rte_zmalloc("vhost device", sizeof(*dev),
					     RTE_CACHE_LINE_SIZE);
	if(dev == NULL)
		return -1;

	/* Initialize device information */
	dev->virtual = 1;
	dev->id = vid;
	dev->state = DEVICE_READY;

	/* Signal the forwarding thread to unlock the device list */
	Dev_list_update = 1;

	pthread_mutex_lock(&Dev_list_mutex);
	TAILQ_INSERT_TAIL(&Dev_list, dev, dev_entry);
	Dev_list_update = 0;
	pthread_mutex_unlock(&Dev_list_mutex);
	RTE_LOG(INFO, LSWITCH_CONFIG,
		"Virtual device %d is added to the device list\n", dev->id);
	return 0;
}


/*
 * A callback function when a vdev is removed.
 */
static void
destroy_vdev_callback(int vid)
{
	struct dev_info *dev = NULL;
	struct ether_addr *key;
	int ret;

	TAILQ_FOREACH(dev, &Dev_list, dev_entry) {
		if(!dev->virtual)
			continue;
		if(dev->id == vid)
			break;
	}
	if(dev == NULL)
		return;

	/* Signal the forwarding thread not to interack with this device */
	dev->state = DEVICE_CLOSING;

	/* Rignal the forwarding thread to unlock the device list */
	Dev_list_update = 1;
	pthread_mutex_lock(&Dev_list_mutex);
	TAILQ_REMOVE(&Dev_list, dev, dev_entry);

	/* Remove all mappings from some source addresses to the device */
	for(uint32_t i = 0; i < NUM_HASH_ENTRIES; i++) {
		if(Output_table[i] != NULL && Output_table[i]->virtual &&
		   Output_table[i]->id == vid) {
			rte_hash_get_key_with_position(Mac_output_map, i,
						       (void **)&key);
			ret = rte_hash_del_key(Mac_output_map, key);
			if(ret < 0)
				RTE_LOG(INFO, LSWITCH_CONFIG,
					"Couldn't delete MAC: "
					"%02" PRIx8 "%02" PRIx8 "%02" PRIx8
					"%02" PRIx8 "%02" PRIx8 "%02" PRIx8
					"from the hash table\n",
					key->addr_bytes[0], key->addr_bytes[1],
					key->addr_bytes[2], key->addr_bytes[3],
					key->addr_bytes[4], key->addr_bytes[5]);
			else
				RTE_LOG(INFO, LSWITCH_CONFIG, "MAC: "
					"%02" PRIx8 "%02" PRIx8 "%02" PRIx8
					"%02" PRIx8 "%02" PRIx8 "%02" PRIx8
					" is removed from the hash table\n",
					key->addr_bytes[0], key->addr_bytes[1],
					key->addr_bytes[2], key->addr_bytes[3],
					key->addr_bytes[4], key->addr_bytes[5]);
			Output_table[i] = NULL;
		}
	}
	Dev_list_update = 0;
	pthread_mutex_unlock(&Dev_list_mutex);
	RTE_LOG(INFO, LSWITCH_CONFIG,
		"Virtual device %d is deleted from the device list\n", dev->id);
	rte_free(dev);
}


/*
 * Hash function maps a MAC address to 8-bits hash value.
 */
static uint32_t
mac_to_uchar(const void *data, uint32_t data_len, uint32_t init_val)
{
	const uint8_t *d = (const uint8_t *)data;
	for(int i = 0; i < (int)data_len; i++) {
		init_val ^= d[i];
	}
	return init_val;
}


/*
 * This function learns source MAC address from received packet. The address is
 * a key of the mac-to-output hash table. A value associated with the key is an
 * index of the output table, each element of which pointers to an output port.
 */
static int
learn_mac_address(struct rte_mbuf *mbuf, struct dev_info *s_dev)
{
	struct ether_hdr *pkt_hdr;
	int ret;

	pkt_hdr = rte_pktmbuf_mtod(mbuf, struct ether_hdr *);
	ret = rte_hash_add_key(Mac_output_map, &pkt_hdr->s_addr);
	if(ret < 0) {
		RTE_LOG(INFO, LSWITCH_CONFIG,
			"Cannot add an entry to mac-output lookup table "
			"(error %d)\n", ret);
		return -1;
	}

	if(Output_table[ret] == NULL ||
	   Output_table[ret]->virtual != s_dev->virtual ||
	   Output_table[ret]->id != s_dev->id) {
		Output_table[ret] = s_dev;
		RTE_LOG(INFO, LSWITCH_CONFIG, "MAC: "
			"%02" PRIx8 "%02" PRIx8 "%02" PRIx8
			"%02" PRIx8 "%02" PRIx8 "%02" PRIx8
			" is mapped to %s device with id %d\n",
			pkt_hdr->s_addr.addr_bytes[0], pkt_hdr->s_addr.addr_bytes[1],
			pkt_hdr->s_addr.addr_bytes[2], pkt_hdr->s_addr.addr_bytes[3],
			pkt_hdr->s_addr.addr_bytes[4], pkt_hdr->s_addr.addr_bytes[5],
			(s_dev->virtual) ? "virtual" : "physical", s_dev->id);
	}
	return 0;
}


/*
 * This function forwards a given packet according to the following conditions:
 * 1) If the packet's destination address in not recognizable, the packet is
 *    broadcast to all ports/devices except the packet's incoming one.
 * 2) Otherwise, the packet is unicast according to its destination address.
 */
static void
forward_packet(struct rte_mbuf *mbuf, struct dev_info *s_dev)
{
	struct ether_hdr *pkt_hdr;
	struct dev_info *dev;
	struct rte_mbuf *tbuf;
	int ret;

	/* Get the Ethernet header and find destination output */
	pkt_hdr = rte_pktmbuf_mtod(mbuf, struct ether_hdr *);
	ret = rte_hash_lookup(Mac_output_map, &pkt_hdr->d_addr);

	/* Broadcast */
	if(ret < 0) {
		TAILQ_FOREACH(dev, &Dev_list, dev_entry) {
			if(dev == s_dev)
				continue;

			if(dev->virtual) {
				if(unlikely(dev->state == DEVICE_CLOSING))
					continue;
				rte_vhost_enqueue_burst(dev->id, VIRTIO_RXQ,
							&mbuf, 1);
			}
			else {
				tbuf = rte_pktmbuf_clone(mbuf, Mbuf_pool);
				ret = rte_eth_tx_burst(dev->id, 0, &tbuf, 1);
				if(unlikely(ret == 0))
					rte_pktmbuf_free(tbuf);
			}
		}
		rte_pktmbuf_free(mbuf);
		return;
	}

	/* Unicast */
	dev = Output_table[ret];
	if(dev->virtual) {
		if(unlikely(dev->state != DEVICE_CLOSING))
			rte_vhost_enqueue_burst(dev->id, VIRTIO_RXQ, &mbuf, 1);
		rte_pktmbuf_free(mbuf);
	}
	else {
		ret = rte_eth_tx_burst(dev->id, 0, &mbuf, 1);
		if(unlikely(ret == 0))
			rte_pktmbuf_free(mbuf);
	}
}


/*
 * This function unregisters socket files and stop physical devices.
 */
static void cleanup(void)
{
	for(int vid = 0; vid < Nb_sockets; vid++)
		rte_vhost_driver_unregister(&Socket_files[vid * PATH_MAX]);

	for(int portid = 0; portid < Nb_ports; portid++)
		rte_eth_dev_stop(portid);

	pthread_mutex_destroy(&Dev_list_mutex);
	exit(0);
}


/*
 * The main forwarding thread forwards packets most of the time.
 * It only pauses the forwarding when the device list needs to be updated.
 */
static int learning_switch_main(void *arg __rte_unused)
{
	struct rte_mbuf *mbufs[MAX_PKT_BURST];
	uint16_t nb_rcv;
	struct dev_info *dev;

	while(!Force_exit) {
		pthread_mutex_lock(&Dev_list_mutex);

		while(!Dev_list_update && !Force_exit) {
			TAILQ_FOREACH(dev, &Dev_list, dev_entry) {
				if(dev->state == DEVICE_CLOSING)
					continue;

				if(dev->virtual)
					nb_rcv = rte_vhost_dequeue_burst(dev->id,
									 VIRTIO_TXQ,
									 Mbuf_pool, mbufs,
									 MAX_PKT_BURST);
				else
					nb_rcv = rte_eth_rx_burst(dev->id, 0, mbufs,
								  MAX_PKT_BURST);

				for(int n = 0; n < nb_rcv; n++) {
					learn_mac_address(mbufs[n], dev);
					forward_packet(mbufs[n], dev);
				}
			}
		}

		pthread_mutex_unlock(&Dev_list_mutex);
	}
	cleanup();
	return 0;
}


/*
 * This function handles SIGINT signal for program termination.
 */
static void force_exit_handling(__rte_unused int signum)
{
	Force_exit = 1;
}


int main(int argc, char **argv)
{
	int ret;
	mode_t sock_perm;

	/* Initialize EAL */
	ret = rte_eal_init(argc, argv);
	if(ret < 0)
		rte_exit(EXIT_FAILURE, "Error with EAL initialization\n");

	argc -= ret;
	argv += ret;

	ret = parse_socket_paths(argc, argv);
	if(ret < 0)
		rte_exit(EXIT_FAILURE, "Invalid switch arguments\n");

	/* Register SIGINT for termination and cleaning up */
	signal(SIGINT, force_exit_handling);

	/* Create mbuf pool */
	Nb_ports = rte_eth_dev_count();
	Mbuf_pool = rte_pktmbuf_pool_create("MBUF_POOL",
					    NUM_MBUFS * (Nb_ports + Nb_sockets),
					    MBUF_CACHE_SIZE, 0,
					    RTE_MBUF_DEFAULT_BUF_SIZE,
					    rte_socket_id());

	/* Create MAC-address-to-output hash table */
	const struct rte_hash_parameters mac_output_map_params = {
		.name = NULL,
		.entries = NUM_HASH_ENTRIES,
		.key_len = sizeof(struct ether_addr),
		.hash_func = mac_to_uchar,
		.hash_func_init_val = 0,
	};
	Mac_output_map = rte_hash_create(&mac_output_map_params);
	memset(Output_table, 0, sizeof(struct dev_info *) * NUM_HASH_ENTRIES);

	/* Initialize device list*/
	TAILQ_INIT(&Dev_list);
	Dev_list_update = 0;
	if(pthread_mutex_init(&Dev_list_mutex, NULL) != 0) {
		rte_exit(EXIT_FAILURE, "Mutex fuilure\n");
	}

	/* Initialize physical ports */
	for(int portid = 0; portid < Nb_ports; portid++) {
		if(port_init(portid) != 0) {
			rte_exit(EXIT_FAILURE,
				 "Physical port %d initialization failure\n",
				 portid);
		}
	}


	/* Set permission of socket files to 0666 */
	sock_perm = umask(~0666);

	/* Register vhost drivers */
	for(int i = 0; i < Nb_sockets; i++) {
		ret = rte_vhost_driver_register(&Socket_files[i * PATH_MAX], 0);
		if(ret != 0) {
			rte_exit(EXIT_FAILURE, "Vhost register failure\n");
		}
	}

	/* Set process permission back to its original value */
	umask(sock_perm);

	/* Lunch forwarding thread */
	rte_eal_remote_launch(learning_switch_main, NULL, 1);

	/* Register callback for adding and removing virtual devices */
	const struct virtio_net_device_ops virtio_net_device_ops = {
		.new_device =  new_vdev_callback,
		.destroy_device = destroy_vdev_callback,
	};
	rte_vhost_driver_callback_register(&virtio_net_device_ops);

	rte_vhost_driver_session_start();
	return 0;
}
