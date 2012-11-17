/*
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

/**
 * @file   sniffer.c
 * @brief  ROHC sniffer program
 * @author Didier Barvaux <didier@barvaux.org>
 *
 * Objectives:
 *   Easily test the library on a network without affecting it.
 *   Gather compression statistics on a network without affecting it.
 *
 * How it works:
 *   The program sniffs IP packets from a given network interface, and tests
 *   the ROHC library with them. The packets are compressed, then decompressed,
 *   and finally compared with the original IP packets.
 *
 * Statistics:
 *   Some statistics are gathered during the tests. There are printed on the
 *   console. More stats should be added. A better way to export them remains to
 *   be added too.
 *
 * Post-mortem bug analysis:
 *   The program stops (assertion) if compression/decompression/comparison
 *   fails. The last library traces are recorded and printed in case of error.
 *   The last packets are recorded in PCAP files, one per context. This is
 *   also a good idea to run the program with core enabled. Many elements are
 *   thus available to reproduce and fix the discovered problems.
 */

#include "config.h" /* for HAVE_*_H */

/* system includes */
#include <stdio.h>
#include <stdlib.h>
#include <strings.h>
#include <string.h>
#include <net/ethernet.h>
#include <netinet/ip.h>
#include <netinet/ip6.h>
#include <math.h>
#include <errno.h>
#include <stdarg.h>
#include <assert.h>
#include <unistd.h>
#include <signal.h>

/* include for the PCAP library */
#if HAVE_PCAP_PCAP_H == 1
#  include <pcap/pcap.h>
#elif HAVE_PCAP_H == 1
#  include <pcap.h>
#else
#  error "pcap.h header not found, did you specified --enable-rohc-tests \
for ./configure ? If yes, check configure output and config.log"
#endif

/* ROHC includes */
#include <rohc.h>
#include <rohc_comp.h>
#include <rohc_decomp.h>


/** Return the smaller value from the two */
#define min(x, y)  (((x) < (y)) ? (x) : (y))
/** Return the greater value from the two */
#define max(x, y)  (((x) > (y)) ? (x) : (y))

/** The device MTU (TODO: should not be hardcoded) */
#define DEV_MTU 1518

/** The maximal size for the ROHC packets */
#define MAX_ROHC_SIZE  (5 * 1024)

/** The length of the Linux Cooked Sockets header */
#define LINUX_COOKED_HDR_LEN  16

/** The minimum Ethernet length (in bytes) */
#define ETHER_FRAME_MIN_LEN  60


/* prototypes of private functions */

static void usage(void);
static void handle_signals(int signal);

static bool sniff(const int use_large_cid,
                  const unsigned int max_contexts,
                  const char *const device_name)
	__attribute__((nonnull(3)));
static int compress_decompress(struct rohc_comp *comp,
                               struct rohc_decomp *decomp,
                               struct pcap_pkthdr header,
                               unsigned char *packet,
                               int link_len_src,
                               int use_large_cid,
                               pcap_t *handle,
                               pcap_dumper_t *dumpers[],
                               unsigned int *const cid);

static int compare_packets(unsigned char *pkt1, int pkt1_size,
                           unsigned char *pkt2, int pkt2_size);

static void print_rohc_traces(const rohc_trace_level_t level,
                              const rohc_trace_entity_t entity,
                              const int profile,
                              const char *const format,
                              ...)
	__attribute__((format(printf, 4, 5), nonnull(4)));
static int gen_false_random_num(const struct rohc_comp *const comp,
                                void *const user_context)
	__attribute__((nonnull(1)));
static bool rtp_detect_cb(const unsigned char *const ip,
                          const unsigned char *const udp,
                          const unsigned char *const payload,
                          const unsigned int payload_size,
                          void *const rtp_private)
	__attribute__((nonnull(1, 2, 3), warn_unused_result));


/** Whether the application shall stop or not */
static bool stop_program;

/** Whether the application runs in verbose mode or not */
static bool is_verbose;

/** The maximum number of traces to keep */
#define MAX_LAST_TRACES  5000
/** The maximum length of a trace */
#define MAX_TRACE_LEN  300

/** The ring buffer for the last traces */
static char last_traces[MAX_LAST_TRACES][MAX_LAST_TRACES + 1];
/** The index of the first trace */
static int last_traces_first;
/** The index of the last trace */
static int last_traces_last;


/**
 * @brief Main function for the ROHC sniffer application
 *
 * @param argc The number of program arguments
 * @param argv The program arguments
 * @return     The unix return code:
 *              \li 0 in case of success,
 *              \li 1 in case of failure
 */
int main(int argc, char *argv[])
{
	char *cid_type = NULL;
	char *device_name = NULL;
	int max_contexts = ROHC_SMALL_CID_MAX + 1;
	int use_large_cid;
	int args_used;
	int i;

	/* by default, we don't stop */
	stop_program = false;

	/* set to quiet mode by default */
	is_verbose = false;

	/* no traces at the moment */
	for(i = 0; i < MAX_LAST_TRACES; i++)
	{
		last_traces[i][0] = '\0';
	}
	last_traces_first = -1;
	last_traces_last = -1;

	/* parse program arguments, print the help message in case of failure */
	if(argc <= 1)
	{
		usage();
		goto error;
	}

	for(argc--, argv++; argc > 0; argc -= args_used, argv += args_used)
	{
		args_used = 1;

		if(!strcmp(*argv, "-v"))
		{
			/* print version */
			printf("ROHC sniffer program, based on library version %s\n",
			       rohc_version());
			goto error;
		}
		else if(!strcmp(*argv, "-h"))
		{
			/* print help */
			usage();
			goto error;
		}
		else if(!strcmp(*argv, "--verbose"))
		{
			/* enable verbose mode */
			is_verbose = true;
		}
		else if(!strcmp(*argv, "--max-contexts"))
		{
			/* get the maximum number of contexts the test should use */
			max_contexts = atoi(argv[1]);
			args_used++;
		}
		else if(cid_type == NULL)
		{
			/* get the type of CID to use within the ROHC library */
			cid_type = argv[0];
		}
		else if(device_name == NULL)
		{
			/* get the device on which we will capture packets to compress,
			 * then decompress */
			device_name = argv[0];
		}
		else
		{
			/* do not accept more than one filename without option name */
			usage();
			goto error;
		}
	}

	/* check CID type */
	if(!strcmp(cid_type, "smallcid"))
	{
		use_large_cid = 0;

		/* the maximum number of ROHC contexts should be valid */
		if(max_contexts < 1 || max_contexts > (ROHC_SMALL_CID_MAX + 1))
		{
			fprintf(stderr, "the maximum number of ROHC contexts should be "
			        "between 1 and %u\n\n", ROHC_SMALL_CID_MAX + 1);
			usage();
			goto error;
		}
	}
	else if(!strcmp(cid_type, "largecid"))
	{
		use_large_cid = 1;

		/* the maximum number of ROHC contexts should be valid */
		if(max_contexts < 1 || max_contexts > (ROHC_LARGE_CID_MAX + 1))
		{
			fprintf(stderr, "the maximum number of ROHC contexts should be "
			        "between 1 and %u\n\n", ROHC_LARGE_CID_MAX + 1);
			usage();
			goto error;
		}
	}
	else
	{
		fprintf(stderr, "invalid CID type '%s', only 'smallcid' and "
		        "'largecid' expected\n", cid_type);
		goto error;
	}

	/* the source filename is mandatory */
	if(device_name == NULL)
	{
		usage();
		goto error;
	}

	/* set signal handlers */
	signal(SIGINT, handle_signals);
	signal(SIGTERM, handle_signals);
	signal(SIGSEGV, handle_signals);

	/* test ROHC compression/decompression with the packets from the file */
	if(!sniff(use_large_cid, max_contexts, device_name))
	{
		goto error;
	}

	return 0;

error:
	return 1;
}


/**
 * @brief Print usage of the sniffer test application
 */
static void usage(void)
{
	printf("ROHC sniffer tool: test the ROHC library with sniffed traffic\n"
	       "\n"
	       "usage: rohc_sniffer [OPTIONS] CID_TYPE DEVICE\n"
	       "\n"
	       "with:\n"
	       "  CID_TYPE            The type of CID to use among 'smallcid'\n"
	       "                      and 'largecid'\n"
	       "  DEVICE              The name of the network device to use\n"
	       "\n"
	       "options:\n"
	       "  -v                  Print version information and exit\n"
	       "  -h                  Print this usage and exit\n"
	       "  --max-contexts NUM  The maximum number of ROHC contexts to\n"
	       "                      simultaneously use during the test\n"
	       "  --verbose           Make the test more verbose\n");
}


/**
 * @brief Handle UNIX signals
 *
 * @param signal  The received signal
 */
static void handle_signals(int signal)
{
	/* end the program with next captured packet */
	fprintf(stderr, "signal %d catched\n", signal);
	stop_program = true;
}


/**
 * @brief Test the ROHC library with a sniffed flow of IP packets going
 *        through one compressor/decompressor pair
 *
 * @param use_large_cid        Whether the compressor shall use large CIDs
 * @param max_contexts         The maximum number of ROHC contexts to use
 * @param device_name          The name of the network device
 * @return                     Whether the sniffer setup was OK
 */
static bool sniff(const int use_large_cid,
                  const unsigned int max_contexts,
                  const char *const device_name)
{
	char errbuf[PCAP_ERRBUF_SIZE];
	pcap_t *handle;
	int link_layer_type_src;
	int link_len_src;
	struct pcap_pkthdr header;
	unsigned char *packet;
	unsigned int counter;

	struct rohc_comp *comp;
	struct rohc_decomp *decomp;

	pcap_dumper_t *dumpers[max_contexts];

	int ret;
	int i;

	/* statistics */
	unsigned int nb_ok = 0;
	unsigned int nb_bad = 0;
	unsigned int nb_internal_err = 0;
	unsigned int err_comp = 0;
	unsigned int err_decomp = 0;
	unsigned int nb_ref = 0;

	/* init status */
	bool status = false;

	assert(device_name != NULL);

	/* open the network device */
	handle = pcap_open_live(device_name, DEV_MTU, 0, 0, errbuf);
	if(handle == NULL)
	{
		fprintf(stderr, "failed to open network device '%s': %s\n",
		       device_name, errbuf);
		goto error;
	}

	/* link layer in the source dump must be Ethernet */
	link_layer_type_src = pcap_datalink(handle);
	if(link_layer_type_src != DLT_EN10MB &&
	   link_layer_type_src != DLT_LINUX_SLL &&
	   link_layer_type_src != DLT_RAW)
	{
		fprintf(stderr, "link layer type %d not supported in source dump "
		        "(supported = %d, %d, %d)\n", link_layer_type_src, DLT_EN10MB,
		        DLT_LINUX_SLL, DLT_RAW);
		goto close_input;
	}

	if(link_layer_type_src == DLT_EN10MB)
	{
		link_len_src = ETHER_HDR_LEN;
	}
	else if(link_layer_type_src == DLT_LINUX_SLL)
	{
		link_len_src = LINUX_COOKED_HDR_LEN;
	}
	else /* DLT_RAW */
	{
		link_len_src = 0;
	}

	/* create the ROHC compressor */
	comp = rohc_alloc_compressor(max_contexts - 1, 0, 0, 0);
	if(comp == NULL)
	{
		fprintf(stderr, "failed to create the ROHC compressor\n");
		goto close_input;
	}

	/* set the callback for traces on compressor */
	if(!rohc_comp_set_traces_cb(comp, print_rohc_traces))
	{
		fprintf(stderr, "failed to set trace callback for compressor\n");
		goto destroy_comp;
	}

	/* enable profiles */
	rohc_activate_profile(comp, ROHC_PROFILE_UNCOMPRESSED);
	rohc_activate_profile(comp, ROHC_PROFILE_UDP);
	rohc_activate_profile(comp, ROHC_PROFILE_IP);
	rohc_activate_profile(comp, ROHC_PROFILE_UDPLITE);
	rohc_activate_profile(comp, ROHC_PROFILE_RTP);
	rohc_activate_profile(comp, ROHC_PROFILE_ESP);

	/* configure SMALL_CID / LARGE_CID and MAX_CID */
	rohc_c_set_large_cid(comp, use_large_cid);
	rohc_c_set_max_cid(comp, max_contexts - 1);

	/* set the callback for random numbers on compressor */
	if(!rohc_comp_set_random_cb(comp, gen_false_random_num, NULL))
	{
		fprintf(stderr, "failed to set the random numbers callback for "
		        "compressor\n");
		goto destroy_comp;
	}

	/* reset list of RTP ports for compressor */
	if(!rohc_comp_reset_rtp_ports(comp))
	{
		fprintf(stderr, "failed to reset list of RTP ports for compressor\n");
		goto destroy_comp;
	}

	/* set the callback for RTP stream detection */
	if(!rohc_comp_set_rtp_detection_cb(comp, rtp_detect_cb, NULL))
	{
		fprintf(stderr, "failed to set the RTP stream detection callback for "
		        "compressor\n");
		goto destroy_comp;
	}

	/* create the decompressor (bi-directional mode) */
	decomp = rohc_alloc_decompressor(comp);
	if(decomp == NULL)
	{
		fprintf(stderr, "failed to create the decompressor\n");
		goto destroy_comp;
	}

	/* set the callback for traces on decompressor */
	if(!rohc_decomp_set_traces_cb(decomp, print_rohc_traces))
	{
		printf("cannot set trace callback for decompressor\n");
		goto destroy_decomp;
	}

	/* set CID type and MAX_CID for decompressor */
	if(use_large_cid)
	{
		if(!rohc_decomp_set_cid_type(decomp, ROHC_LARGE_CID))
		{
			fprintf(stderr, "failed to set CID type to large CIDs for "
			        "decompressor\n");
			goto destroy_decomp;
		}
		if(!rohc_decomp_set_max_cid(decomp, ROHC_LARGE_CID_MAX))
		{
			fprintf(stderr, "failed to set MAX_CID to %d for decompressor\n",
			        ROHC_LARGE_CID_MAX);
			goto destroy_decomp;
		}
	}
	else
	{
		if(!rohc_decomp_set_cid_type(decomp, ROHC_SMALL_CID))
		{
			fprintf(stderr, "failed to set CID type to small CIDs for "
			        "decompressor\n");
			goto destroy_decomp;
		}
		if(!rohc_decomp_set_max_cid(decomp, ROHC_SMALL_CID_MAX))
		{
			fprintf(stderr, "failed to set MAX_CID to %d for decompressor\n",
			        ROHC_SMALL_CID_MAX);
			goto destroy_decomp;
		}
	}

	/* reset the PCAP dumpers (used to save sniffed packets in several PCAP
	 * files, one per Context ID) */
	bzero(dumpers, sizeof(pcap_dumper_t *) * max_contexts);

	/* for each sniffed packet */
	counter = 0;
	while(!stop_program &&
	      (packet = (unsigned char *) pcap_next(handle, &header)) != NULL)
	{
		unsigned int cid = 0;

		counter++;

		if(counter > 1)
		{
			printf("\r");
		}
		printf("packet #%u", counter);

		/* compress & decompress from compressor to decompressor */
		ret = compress_decompress(comp, decomp, header, packet,
		                          link_len_src, use_large_cid,
		                          handle, dumpers, &cid);
		if(ret == -1)
		{
			err_comp++;
		}
		else if(ret == -2)
		{
			err_decomp++;
		}
		else if(ret == 0)
		{
			nb_ref++;
		}
		else if(ret == 1)
		{
			nb_ok++;
		}
		else if(ret == -3)
		{
			nb_bad++;
		}
		else
		{
			nb_internal_err++;
		}

		/* in case of problem, print recorded traces then die! */
		if(ret != 1)
		{
			fprintf(stderr, "packet #%u, CID %u: stats OK, ERR(COMP), "
			        "ERR(DECOMP), ERR(REF), ERR(BAD), ERR(INTERNAL)\t="
			        "\t%u\t%u\t%u\t%u\t%u\t%u\n",
			        counter, cid, nb_ok, err_comp, err_decomp, nb_ref, nb_bad,
			        nb_internal_err);
			fflush(stderr);

			if(last_traces_first == -1 || last_traces_last == -1)
			{
				fprintf(stderr, "no trace to display\n");
			}
			else
			{
				if(last_traces_first <= last_traces_last)
				{
					fprintf(stderr, "print the last %d traces...\n",
					        last_traces_last - last_traces_first);
					for(i = last_traces_first; i <= last_traces_last; i++)
					{
						fprintf(stderr, "%s", last_traces[i]);
					}
				}
				else
				{
					fprintf(stderr, "print the last %d traces...\n",
					        MAX_LAST_TRACES - last_traces_first +
					        last_traces_last);
					for(i = last_traces_first;
					    i <= MAX_LAST_TRACES + last_traces_last;
					    i++)
					{
						fprintf(stderr, "%s", last_traces[i % MAX_LAST_TRACES]);
					}
				}
			}

			/* we discovered a problem, make the program stop now! */
			assert(0);
		}
	}

	if(stop_program)
	{
		printf("program stopped by signal\n");
	}

	status = true;

	/* close PCAP dumpers */
	for(i = 0; i < max_contexts; i++)
	{
		if(dumpers[i] != NULL)
		{
			printf("close dump file for context with ID %d\n", i);
			pcap_dump_close(dumpers[i]);
		}
	}

destroy_decomp:
	rohc_free_decompressor(decomp);
destroy_comp:
	rohc_free_compressor(comp);
close_input:
	pcap_close(handle);
error:
	return status;
}


/**
 * @brief Compress and decompress one uncompressed IP packet with the given
 *        compressor and decompressor
 *
 * @param comp           The compressor to use to compress the IP packet
 * @param decomp         The decompressor to use to decompress the IP packet
 * @param header         The PCAP header for the packet
 * @param packet         The packet to compress/decompress (link layer included)
 * @param link_len_src   The length of the link layer header before IP data
 * @param use_large_cid  Whether use large CID or not
 * @param handle         The PCAP handler that sniffed the packet
 * @param dumpers        The PCAP dumpers, one per context
 * @param cid            OUT: the CID used for the last packet
 * @return               1 if the process is successful
 *                       0 if the decompressed packet doesn't match the
 *                         original one
 *                       -1 if an error occurs while compressing
 *                       -2 if an error occurs while decompressing
 *                       -3 if the link layer is not Ethernet
 *                       -4 if compression info cannot be retrieved
 */
static int compress_decompress(struct rohc_comp *comp,
                               struct rohc_decomp *decomp,
                               struct pcap_pkthdr header,
                               unsigned char *packet,
                               int link_len_src,
                               int use_large_cid,
                               pcap_t *handle,
                               pcap_dumper_t *dumpers[],
                               unsigned int *const cid)
{
	unsigned char *ip_packet;
	int ip_size;
	static unsigned char output_packet[max(ETHER_HDR_LEN, LINUX_COOKED_HDR_LEN) + MAX_ROHC_SIZE];
	unsigned char *rohc_packet;
	int rohc_size;
	rohc_comp_last_packet_info2_t last_packet_info;
	static unsigned char decomp_packet[MAX_ROHC_SIZE];
	int decomp_size;
	int ret;

	/* check Ethernet frame length */
	if(header.len <= link_len_src || header.len != header.caplen)
	{
		fprintf(stderr, "bad PCAP packet (len = %d, caplen = %d)\n",
		       header.len, header.caplen);
		ret = -3;
		goto error;
	}

	ip_packet = packet + link_len_src;
	ip_size = header.len - link_len_src;
	rohc_packet = output_packet + link_len_src;

	/* check for padding after the IP packet in the Ethernet payload */
	if(link_len_src == ETHER_HDR_LEN && header.len == ETHER_FRAME_MIN_LEN)
	{
		int version;
		int tot_len;

		version = (ip_packet[0] >> 4) & 0x0f;

		if(version == 4)
		{
			struct iphdr *ip = (struct iphdr *) ip_packet;
			tot_len = ntohs(ip->tot_len);
		}
		else
		{
			struct ip6_hdr *ip = (struct ip6_hdr *) ip_packet;
			tot_len = sizeof(struct ip6_hdr) + ntohs(ip->ip6_plen);
		}

		if(tot_len < ip_size)
		{
			fprintf(stderr, "the Ethernet frame has %d bytes of padding after "
			        "the %d byte IP packet!\n", ip_size - tot_len, tot_len);
			ip_size = tot_len;
		}
	}

	/* compress the IP packet */
	rohc_size = rohc_compress(comp, ip_packet, ip_size,
	                          rohc_packet, MAX_ROHC_SIZE);
	if(rohc_size <= 0)
	{
		pcap_dumper_t *dumper;

		fprintf(stderr, "compression failed\n");
		ret = -1;

		/* open the new dumper */
		dumper = pcap_dump_open(handle, "./dump_stream_default.pcap");
		if(dumper == NULL)
		{
			fprintf(stderr, "failed to open new dump file '%s'\n",
			        "./dump_stream_default.pcap");
			assert(0);
			goto error;
		}

		/* dump the IP packet */
		fprintf(stderr, "dump packet in file '%s'\n",
		        "./dump_stream_default.pcap");
		pcap_dump((u_char *) dumper, &header, packet);

		fprintf(stderr, "close dump file\n");
		pcap_dump_close(dumper);

		goto error;
	}

	/* get some statistics about the last compressed packet */
	last_packet_info.version_major = 0;
	last_packet_info.version_minor = 0;
	ret = rohc_comp_get_last_packet_info2(comp, &last_packet_info);
	if(ret != ROHC_OK)
	{
		fprintf(stderr, "failed to get compression info\n");
		ret = -4;
		goto error;
	}

	/* open a new dumper if none exists or the stream changed */
	if(last_packet_info.is_context_init)
	{
		char dump_filename[1024];

		snprintf(dump_filename, 1024, "./dump_stream_cid_%u.pcap",
		         last_packet_info.context_id);
		/* TODO: check result */

		/* close the previous dumper and remove its file if one was opened */
		if(dumpers[last_packet_info.context_id] != NULL)
		{
			printf("replace dump file '%s' for context with ID %u\n",
			       dump_filename, last_packet_info.context_id);
			pcap_dump_close(dumpers[last_packet_info.context_id]);
			unlink(dump_filename);
			/* TODO: check result */
		}

		/* open the new dumper */
		dumpers[last_packet_info.context_id] =
			pcap_dump_open(handle, dump_filename);
		if(dumpers[last_packet_info.context_id] == NULL)
		{
			fprintf(stderr, "failed to open new dump file '%s' for context "
			        "with ID %u\n", dump_filename, last_packet_info.context_id);
			assert(0);
			goto error;
		}
	}

	/* dump the IP packet */
	pcap_dump((u_char *) dumpers[last_packet_info.context_id],
	          &header, packet);

	/* record the CID */
	*cid = last_packet_info.context_id;

	/* decompress the ROHC packet */
	decomp_size = rohc_decompress(decomp,
	                              rohc_packet, rohc_size,
	                              decomp_packet, MAX_ROHC_SIZE);
	if(decomp_size <= 0)
	{
		fprintf(stderr, "decompression failed\n");
		ret = -2;
		goto error;
	}

	/* compare the decompressed packet with the original one */
	if(!compare_packets(ip_packet, ip_size, decomp_packet, decomp_size))
	{
		fprintf(stderr, "comparison with original packet failed\n");
		ret = 0;
	}
	else
	{
		/* comparison is OK */
		ret = 1;
	}

error:
	return ret;
}


/**
 * @brief Compare two network packets and print differences if any
 *
 * @param pkt1      The first packet
 * @param pkt1_size The size of the first packet
 * @param pkt2      The second packet
 * @param pkt2_size The size of the second packet
 * @return          Whether the packets are equal or not
 */
static int compare_packets(unsigned char *pkt1, int pkt1_size,
                           unsigned char *pkt2, int pkt2_size)
{
	int valid = 1;
	int min_size;
	int i, j, k;
	char str1[4][7], str2[4][7];
	char sep1, sep2;

	/* do not compare more than the shortest of the 2 packets */
	min_size = min(pkt1_size, pkt2_size);

	/* do not compare more than 180 bytes to avoid huge output */
	min_size = min(180, min_size);

	/* if packets are equal, do not print the packets */
	if(pkt1_size == pkt2_size && memcmp(pkt1, pkt2, pkt1_size) == 0)
	{
		goto skip;
	}

	/* packets are different */
	valid = 0;

	printf("------------------------------ Compare ------------------------------\n");
	printf("--------- reference ----------         ----------- new --------------\n");

	if(pkt1_size != pkt2_size)
	{
		printf("packets have different sizes (%d != %d), compare only the %d "
		       "first bytes\n", pkt1_size, pkt2_size, min_size);
	}

	j = 0;
	for(i = 0; i < min_size; i++)
	{
		if(pkt1[i] != pkt2[i])
		{
			sep1 = '#';
			sep2 = '#';
		}
		else
		{
			sep1 = '[';
			sep2 = ']';
		}

		sprintf(str1[j], "%c0x%.2x%c", sep1, pkt1[i], sep2);
		sprintf(str2[j], "%c0x%.2x%c", sep1, pkt2[i], sep2);

		/* make the output human readable */
		if(j >= 3 || (i + 1) >= min_size)
		{
			for(k = 0; k < 4; k++)
			{
				if(k < (j + 1))
				{
					printf("%s  ", str1[k]);
				}
				else /* fill the line with blanks if nothing to print */
				{
					printf("        ");
				}
			}

			printf("      ");

			for(k = 0; k < (j + 1); k++)
			{
				printf("%s  ", str2[k]);
			}

			printf("\n");

			j = 0;
		}
		else
		{
			j++;
		}
	}

	printf("----------------------- packets are different -----------------------\n");

skip:
	return valid;
}


/**
 * @brief Callback to print traces of the ROHC library
 *
 * @param level    The priority level of the trace
 * @param entity   The entity that emitted the trace among:
 *                  \li ROHC_TRACE_COMPRESSOR
 *                  \li ROHC_TRACE_DECOMPRESSOR
 * @param profile  The ID of the ROHC compression/decompression profile
 *                 the trace is related to
 * @param format   The format string of the trace
 */
static void print_rohc_traces(const rohc_trace_level_t level,
                              const rohc_trace_entity_t entity,
                              const int profile,
                              const char *format, ...)
{
	const char *level_descrs[] =
	{
		[ROHC_TRACE_DEBUG]   = "DEBUG",
		[ROHC_TRACE_INFO]    = "INFO",
		[ROHC_TRACE_WARNING] = "WARNING",
		[ROHC_TRACE_ERROR]   = "ERROR"
	};

	if(level >= ROHC_TRACE_WARNING || is_verbose)
	{
		va_list args;
		fprintf(stdout, "[%s] ", level_descrs[level]);
		va_start(args, format);
		vfprintf(stdout, format, args);
		va_end(args);
	}

	if(last_traces_last == -1)
	{
		last_traces_last = 0;
	}
	else
	{
		last_traces_last = (last_traces_last + 1) % MAX_LAST_TRACES;
	}
	{
		va_list args;
		va_start(args, format);
		vsnprintf(last_traces[last_traces_last], MAX_TRACE_LEN + 1, format, args);
		/* TODO: check return code */
		last_traces[last_traces_last][MAX_TRACE_LEN] = '\0';
		va_end(args);
	}
	if(last_traces_first == -1)
	{
		last_traces_first = 0;
	}
	else if(last_traces_first == last_traces_last)
	{
		last_traces_first = (last_traces_first + 1) % MAX_LAST_TRACES;
	}
}


/**
 * @brief Generate a false random number for testing the ROHC library
 *
 * @param comp          The ROHC compressor
 * @param user_context  Should always be NULL
 * @return              Always 0
 */
static int gen_false_random_num(const struct rohc_comp *const comp,
                                void *const user_context)
{
	assert(comp != NULL);
	assert(user_context == NULL);
	return 0;
}


/**
 * @brief The detection callback which do detect RTP stream
 *
 * @param ip           The inner ip packet
 * @param udp          The udp header of the packet
 * @param payload      The payload of the packet
 * @param payload_size The size of the payload (in bytes)
 * @return             1 if the packet is an RTP packet, 0 otherwise
 */
static bool rtp_detect_cb(const unsigned char *const ip,
                          const unsigned char *const udp,
                          const unsigned char *const payload,
                          const unsigned int payload_size,
                          void *const rtp_private)
{
	const uint16_t sip_port = 5060;
	uint16_t udp_sport;
	uint16_t udp_dport;
	uint16_t udp_len;
	uint8_t rtp_pt;
	int is_rtp = 0;

	assert(ip != NULL);
	assert(udp != NULL);
	assert(payload != NULL);
	assert(rtp_private == NULL);

	/* retrieve UDP source and destination ports and UDP length */
	memcpy(&udp_sport, udp, sizeof(uint16_t));
	memcpy(&udp_dport, udp + 2, sizeof(uint16_t));
	memcpy(&udp_len, udp + 4, sizeof(uint16_t));

	/* SIP (UDP/5060) is not RTP */
	if(ntohs(udp_sport) == sip_port && ntohs(udp_dport) == sip_port)
	{
		goto not_rtp;
	}

	/* the UDP destination port of RTP packet is even (the RTCP destination
	 * port are RTP destination port + 1, so it is odd) */
	if((udp_dport % 2) != 0)
	{
		goto not_rtp;
	}

	/* UDP Length shall not be too large */
	if(ntohs(udp_len) > 200)
	{
		goto not_rtp;
	}

	/* UDP payload shall at least contain the smallest RTP header */
	if(payload_size < 12)
	{
		goto not_rtp;
	}

	/* RTP version bits shall be 2 */
	if(((payload[0] >> 6) & 0x3) != 0x2)
	{
		goto not_rtp;
	}

	/* RTP payload type shall be GSM (0x03), ITU-T G.723 (0x04),
	 * ITU-T G.729 (0x12) or telephony-event (0x65) */
	rtp_pt = payload[1] & 0x7f;
	if(rtp_pt != 0x03 && rtp_pt != 0x04 && rtp_pt != 0x12 && rtp_pt != 0x65)
	{
		goto not_rtp;
	}

	/* we think that the UDP packet is a RTP packet */
	is_rtp = 1;

not_rtp:
	return is_rtp;
}
