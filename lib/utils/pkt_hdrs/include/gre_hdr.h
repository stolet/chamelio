#ifndef GRE_HDR_H_
#define GRE_HDR_H_

#include "utils.h"
#include "ip_hdr.h"
#include "eth_hdr.h"

/* Gets GRE checksum */
#define GREH_C(hdr) ((hdr)->_c_k_s_ver >> 15)
/* Gets key present field */
#define GREH_K(hdr) ((hdr)->_c_k_s_ver & 0b0010000000000000)
/* Gets sequence number field */
#define GREH_S(hdr) ((hdr)->_c_k_s_ver & 0b0001000000000000)
/* Gets version field */
#define GREH_V(hdr) ((hdr)->_c_k_s_ver & 0b0000000000000111)

/* Sets checksum, key present, seq num, and version */
#define GREH_CKSV_SET(hdr, c, k, s, v) (hdr)->_c_k_s_ver = \
    t_beui16(((c) << 15) | ((k) << 13) | ((s) << 12) | (v))

#define GRE_PROTO_IP 0x0800

struct gre_hdr {
  /* checksum / key present / seq num / reserved / version */
  beui16_t _c_k_s_ver;
  /* protocol */
  beui16_t proto;
  /* key number used to identify a flow */
  beui32_t key;
} __attribute__ ((packed));

struct gre_pkt {
  struct eth_hdr eth;
  struct ip_hdr outer_ip;
  struct gre_hdr gre;
  struct ip_hdr inner_ip;
};

#endif