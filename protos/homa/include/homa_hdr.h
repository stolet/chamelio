#ifndef CHAM_HOMA_HDR_H
#define CHAM_HOMA_HDR_H

#include "ip_hdr.h"

/* Experimental IPv4 protocol number. */
#define IP_PROTO_HOMA 253
/* Unscheduled or scheduled message data. */
#define HOMA_DATA 0x10
/* Single DATA packet at the physical MTU of 1500 bytes. */
#define HOMA_MAX_PAYLOAD 1420

/* Homa common header, including the TCP-compatible prefix. */
struct homa_common_hdr {
  /* Source port. */
  beui16_t sport;
  /* Destination port. */
  beui16_t dport;
  /* Unused TCP sequence and acknowledgment fields. */
  __u32 unused[2];
  /* DATA header length in four-byte words, in the high four bits. */
  __u8 doff;
  /* Homa packet type. */
  __u8 type;
  /* Packet sequence number. */
  __u16 seq;
  /* Reserved checksum field; Homa does not use it. */
  __u16 checksum;
  /* Unused TCP urgent pointer. */
  __u16 unused4;
  /* Sender's RPC ID; even for a client, odd for a server. */
  beui64_t sender_id;
} __attribute__((packed));

/* Complete DATA header for a single message segment. */
struct homa_data_hdr {
  /* Common Homa fields. */
  struct homa_common_hdr common;
  /* Total message bytes. */
  beui32_t message_length;
  /* Bytes authorized without another grant. */
  beui32_t incoming;
  /* Unscheduled priority cutoff version. */
  beui16_t cutoff_version;
  /* Nonzero for a retransmitted segment. */
  __u8 retransmit;
  /* Reserved byte. */
  __u8 unused;
  /* Segment offset within the message. */
  beui32_t offset;
  /* Payload bytes in this segment. */
  beui32_t segment_length;
  /* Piggybacked completion acknowledgment; zero until step 3. */
  beui64_t ack_id;
  /* Acknowledged client's port. */
  beui16_t ack_sport;
  /* Acknowledged server's port. */
  beui16_t ack_dport;
} __attribute__((packed));

/* IPv4 and Homa DATA headers, followed by message bytes. */
struct homa_pkt {
  /* IPv4 header. */
  struct ip_hdr ip;
  /* Homa DATA header. */
  struct homa_data_hdr data;
} __attribute__((packed));

STATIC_ASSERT(sizeof(struct homa_common_hdr) == 28, homa_common_hdr_size);
STATIC_ASSERT(sizeof(struct homa_data_hdr) == 60, homa_data_hdr_size);

#endif
