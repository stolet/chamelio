#ifndef TCP_HDR_H_
#define TCP_HDR_H_

#include "utils.h"
#include "eth_hdr.h"
#include "ip_hdr.h"

#define TAS_TCP_FIN 0x01U
#define TAS_TCP_SYN 0x02U
#define TAS_TCP_RST 0x04U
#define TAS_TCP_PSH 0x08U
#define TAS_TCP_ACK 0x10U
#define TAS_TCP_URG 0x20U
#define TAS_TCP_ECE 0x40U
#define TAS_TCP_CWR 0x80U
#define TAS_TCP_NS  0x100U

#define TAS_TCP_FLAGS 0x1ffU

/* Length of the TCP header, excluding options. */
#define TCP_HLEN 20
/* Length of TCP options */
#define TCP_TS_OPT_LEN 12

#define TCPH_RAW(phdr) ((__u16) __builtin_bswap16((phdr)->_hdrlen_rsvd_flags))
#define TCPH_HDRLEN(phdr) (TCPH_RAW(phdr) >> 12)
#define TCPH_FLAGS(phdr)  (TCPH_RAW(phdr) & TAS_TCP_FLAGS)

#define TCPH_HDRLEN_SET(phdr, len) \
  ((phdr)->_hdrlen_rsvd_flags = __builtin_bswap16(((__u16) (len) << 12) | \
      TCPH_FLAGS(phdr)))
#define TCPH_FLAGS_SET(phdr, flags) \
  ((phdr)->_hdrlen_rsvd_flags = __builtin_bswap16((TCPH_RAW(phdr) & \
      ~((__u16) TAS_TCP_FLAGS)) | ((__u16) (flags) & TAS_TCP_FLAGS)))
#define TCPH_HDRLEN_FLAGS_SET(phdr, len, flags) \
  ((phdr)->_hdrlen_rsvd_flags = __builtin_bswap16(((__u16) (len) << 12) | \
      ((__u16) (flags) & TAS_TCP_FLAGS)))

#define TCPH_SET_FLAG(phdr, flags) \
  ((phdr)->_hdrlen_rsvd_flags = __builtin_bswap16(TCPH_RAW(phdr) | \
      ((__u16) (flags) & TAS_TCP_FLAGS)))
#define TCPH_UNSET_FLAG(phdr, flags) \
  ((phdr)->_hdrlen_rsvd_flags = __builtin_bswap16(TCPH_RAW(phdr) & \
      ~((__u16) (flags) & TAS_TCP_FLAGS)))

#define TCP_TCPLEN(seg) ((seg)->len + \
    ((TCPH_FLAGS((seg)->tcphdr) & (TAS_TCP_FIN | TAS_TCP_SYN)) != 0))

/** This returns a TCP header option for MSS in an u32_t */
#define TCP_BUILD_MSS_OPTION(mss) \
  __builtin_bswap32(0x02040000U | ((__u32) (mss) & 0xFFFFU))

#define TCP_BUILD_SACK_OPTION __builtin_bswap32(0x04020101U)

/* TCP header specified by RFC 9293 */
struct tcp_hdr {
  /* Src prot */
  beui16_t src;
  /* Destination port */
  beui16_t dest;
  /* Sequence number */
  beui32_t seqno;
  /* Acknowledgment number */
  beui32_t ackno;
  /* Header length and reserved flags */
  __u16 _hdrlen_rsvd_flags;
  /* Flow control window */
  beui16_t wnd;
  /* Checksum */
  __u16 chksum;
  /* Urgent pointer */
  beui16_t urgp;
} __attribute__((packed));

#define TCP_OPT_TIMESTAMP 8

/* TCP timestamp option */
struct tcp_timestamp_opt {
  /* Option kind */
  __u8 kind;
  /* Option length */
  __u8 length;
  /* Current val of the clock of TCP sending the option */
  beui32_t ts_val;
  /* Echoes recently received ts_val sent by the remote TCP */
  beui32_t ts_ecr;
  /* Padding because TCP options must be 12 bytes in length */
  __u16 pad;
} __attribute__((packed));

/* Regular TCP packet */
struct tcp_pkt {
  /* Ethernet header */
  struct eth_hdr eth;
  /* IP header */
  struct ip_hdr ip;
  /* TCP header */
  struct tcp_hdr tcp;
} __attribute__ ((packed));

/* Inner layer of encapsulated TCP packet */
struct tcp_pkt_inner {
  /* IP header */
  struct ip_hdr ip;
  /* TCP header */
  struct tcp_hdr tcp;
} __attribute__ ((packed));

#endif
