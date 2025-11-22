#include <stdio.h>
#include <linux/types.h>
#include <arpa/inet.h>
#include <string.h>

#include "eth_hdr.h"
#include "ip_hdr.h"
#include "udp_hdr.h"

static inline const char *ip4_to_str(ip_addr_t a, char buf[INET_ADDRSTRLEN]);
static inline const char *eth_type_name(uint16_t t);
static inline const char *ip_proto_name(uint8_t p);
static inline void mac_to_str(const struct eth_addr *a, char out[18]);
static inline const char *ecn_name(__u8 e);

void log_eth(const struct eth_hdr *eth)
{
  char smac[18], dmac[18];
  mac_to_str(&eth->src, smac);
  mac_to_str(&eth->dst, dmac);

  __u16 type = f_beui16(eth->type);

  fprintf(stderr,
          "[ETH] dst=%s src=%s type=0x%04x (%s)\n",
          dmac, smac, type, eth_type_name(type));
}

void log_ip(const struct ip_hdr *ip)
{
  const __u8 ver  = IPH_V(ip);
  const __u8 ihl  = IPH_HL(ip);               
  const __u8 tos  = IPH_TOS(ip);
  const __u8 ecn  = IPH_ECN(ip) & 0x3;

  const __u16 tot_len = f_beui16(ip->len);
  const __u16 ident   = f_beui16(ip->id);
  const __u16 offraw  = f_beui16(ip->offset);

  /* IPv4 flags/fragment offset */
  const __u16 flags   = (offraw & 0xE000u) >> 13;    
  const __u16 df      = (flags & 0x2u) ? 1u : 0u;     
  const __u16 mf      = (flags & 0x1u) ? 1u : 0u;     
  const __u16 frag_of = (offraw & 0x1FFFu) * 8u;      

  const __u8  ttl     = ip->ttl;
  const __u8  proto   = ip->proto;
  const __u16 csum    = ip->chksum;              

  char srcbuf[INET_ADDRSTRLEN], dstbuf[INET_ADDRSTRLEN];
  ip4_to_str(ip->src, srcbuf);
  ip4_to_str(ip->dst, dstbuf);

  fprintf(stderr,
          "[IP ] v=%u ihl=%uB tos=0x%02x (%s) len=%u id=0x%04x "
          "flags:DF=%u,MF=%u frag_off=%uB ttl=%u proto=%u (%s) "
          "chksum=0x%04x src=%s dst=%s\n",
          ver, (unsigned)(ihl * 4), tos, ecn_name(ecn),
          (unsigned)tot_len, (unsigned)ident,
          (unsigned)df, (unsigned)mf, (unsigned)frag_of,
          (unsigned)ttl, (unsigned)proto, ip_proto_name(proto),
          (unsigned)csum, srcbuf, dstbuf);
}

void log_udp(const struct udp_hdr *udp)
{
  const __u16 sport = f_beui16(udp->src);
  const __u16 dport = f_beui16(udp->dst);
  const __u16 len   = f_beui16(udp->len);
  const __u16 csum  = udp->chksum;

  fprintf(stderr,
          "[UDP] sport=%u dport=%u len=%u chksum=0x%04x\n",
          (unsigned)sport, (unsigned)dport, (unsigned)len, (unsigned)csum);
}

void log_udp_pkt(const struct udp_pkt *p)
{
  log_eth(&p->eth);
  log_ip(&p->ip);
  log_udp(&p->udp);
}

static inline const char *ip4_to_str(ip_addr_t a, char buf[INET_ADDRSTRLEN])
{
  struct in_addr ina;
  memcpy(&ina.s_addr, &a, sizeof ina.s_addr);
  return inet_ntop(AF_INET, &ina, buf, INET_ADDRSTRLEN);
}

static inline void mac_to_str(const struct eth_addr *a, char out[18])
{
  const unsigned char *b = a->addr;
  snprintf(out, 18, "%02x:%02x:%02x:%02x:%02x:%02x",
           b[0], b[1], b[2], b[3], b[4], b[5]);
}

static inline const char *eth_type_name(__u16 t)
{
  switch (t) {
    case ETH_TYPE_IP:  return "IPv4";
    case ETH_TYPE_ARP: return "ARP";
    default:           return "Unknown";
  }
}

static inline const char *ip_proto_name(__u8 p)
{
  switch (p) {
    case IP_PROTO_IP:      return "IP";
    case IP_PROTO_ICMP:    return "ICMP";
    case IP_PROTO_IGMP:    return "IGMP";
    case IP_PROTO_IPENCAP: return "IPENCAP";
    case IP_PROTO_TCP:     return "TCP";
    case IP_PROTO_DCCP:    return "DCCP";
    case IP_PROTO_GRE:     return "GRE";
    case IP_PROTO_UDP:     return "UDP";
    case IP_PROTO_UDPLITE: return "UDPLITE";
    default:               return "Unknown";
  }
}

static inline const char *ecn_name(__u8 e)
{
  switch (e & 0x3) {
    case CHAM_IP_ECN_NONE: return "ECN:none";
    case CHAM_IP_ECN_ECT0: return "ECN:ECT(0)";
    case CHAM_IP_ECN_ECT1: return "ECN:ECT(1)";
    case CHAM_IP_ECN_CE:   return "ECN:CE";
    default:               return "ECN:?";
  }
}