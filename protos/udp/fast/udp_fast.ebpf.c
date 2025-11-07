#include <linux/bpf.h>
#include <bpf/bpf_helpers.h>
#include <bpf/bpf_endian.h>

#include "cham_fast.h"
#include "udp.h"
#include "udp_fast.h"
#include "udp_queue_types.h"
#include "utils.h"

#define SOCK_MAP_IDX 0

static __always_inline __u16 ipv4_checksum(struct ip_hdr *ip);
static __always_inline __u16 udp_checksum(struct ip_hdr *ip, 
    struct udp_hdr *udp);
static __always_inline struct udp_sock * udp_sock_find(struct cham_map *maps,
    __u32 remote_ip_be, __u16 remote_port_be);
    
/* Add these functions as helpers */
static void * (*queue_tail)(struct equeue *q) = (void *) 1001;
static int (*queue_enqueue)(struct equeue *q, __u8 type) = (void *) 1002;
static void * (*bpf_memcpy)(void *src, void *dst, size_t len) = (void *) 1003;
    
SEC("chamelio/event_rx")
int event_rx(struct cham_ebpf_ctx *ctx)
{
int ret;
  __u16 ip_hdrs_len, ip_total_len, udp_len, payload_len;
  __u64 mac_src_val, mac_dst_val;
  struct eth_hdr *eth;
  struct ip_hdr  *ip;
  struct udp_hdr *udp;
  void *payload, *pkt;

  struct udp_sock *sock;
  struct equeue *q;
  struct udp_queue_bump_entry *qe;
  struct udp_queue_bump_app_rx *bump;

  __u8 *rx_base;
  __u32 free_bytes;
  __u32 tail;
  __u32 part;

  __u16 ip_saved_chksum, ip_comp_chksum;
  __u16 udp_saved_chksum, udp_comp_chksum;

  sock = NULL;
  pkt = ctx->pkt;
  
  /* Parse ETH header */
  eth = (struct eth_hdr *) pkt;
  if (f_beui16(eth->type) != ETH_TYPE_IP)
    return -1;
    
  __builtin_memcpy(&mac_src_val, &eth->src, ETH_ADDR_LEN);
  __builtin_memcpy(&mac_dst_val, &eth->dst, ETH_ADDR_LEN);

  /* Parse IP header */
  ip = (struct ip_hdr *) ((__u8 *) pkt + sizeof(struct eth_hdr));
  if (IPH_V(ip) != 4 || IPH_HL(ip) < 5)
    return -1;

  ip_hdrs_len  = (__u16) (IPH_HL(ip) * 4);
  ip_total_len = f_beui16(ip->len);

  if (ip->proto != IP_PROTO_UDP)
    return -1;

  /* Drop fragmented IPv4 for now */
  if (f_beui16(ip->offset) & 0x3FFF)
    return -1;

  if (ip_total_len < ip_hdrs_len + (__u16) sizeof(struct udp_hdr))
    return -1;

  /* Verify IPv4 header checksum */
  ip_saved_chksum = ip->chksum;
  ip->chksum = 0;
  ip_comp_chksum = ipv4_checksum((void *) ip);
  ip->chksum = ip_saved_chksum;
  if (ip_comp_chksum != ip_saved_chksum)
    return -1;

  /* Parse UDP header */
  udp = (struct udp_hdr *) ((__u8 *) ip + ip_hdrs_len);
  udp_len = f_beui16(udp->len);
  if (udp_len < sizeof(struct udp_hdr))
    return -1;
  
  if (ip_total_len < ip_hdrs_len + udp_len)
    return -1;

  /* Verify UDP checksum for IPv4 (0 means “no checksum”) */
  udp_saved_chksum = udp->chksum;
  if (udp_saved_chksum != 0)
  {
    udp->chksum = 0;
    udp_comp_chksum = udp_checksum((void *) ip, (void *) udp);
    udp->chksum = udp_saved_chksum;
    
    if (udp_comp_chksum != udp_saved_chksum)
      return -1;
  }

  /* Lookup socket */
  __u32 src_ip_be  = bpf_htonl(f_beui32(ip->src));
  __u16 src_prt_be = bpf_ntohs(f_beui16(udp->src));
  sock = udp_sock_find(ctx->maps, src_ip_be, src_prt_be);
  
  if (sock == NULL)
    return -1;

  /* Copy payload */
  payload_len = (__u16) (udp_len - sizeof(struct udp_hdr));
  payload = (void *) ((__u8 *) udp + sizeof(struct udp_hdr));
  rx_base = (__u8 *) ctx->shm_base + sock->rx_off;
  free_bytes = sock->rx_len - sock->rx_avail;

  if (payload_len > free_bytes)
    return -1;
  
  tail = sock->rx_head + sock->rx_avail;
  if (tail >= sock->rx_len)
    tail -= sock->rx_len;

  if (tail + payload_len <= sock->rx_len)
  {
    // bpf_probe_read(rx_base + tail, payload_len, payload);
    bpf_memcpy(rx_base + tail, payload, payload_len);
  }
  else
  {
    part = sock->rx_len - tail;
    // bpf_probe_read(rx_base + tail, part, payload);
    // bpf_probe_read(rx_base, payload_len - part, (__u8 *) payload + part);
    bpf_memcpy(rx_base + tail, payload, part);
    bpf_memcpy(rx_base, (__u8 *) payload + part, payload_len - part);
  }

  /* Publish bytes for the consumer */
  sock->rx_avail += payload_len;

  /* Send bump to applocation */
  q = &ctx->equeues[sock->app_bump_qid].eq;
  qe = queue_tail(q);
  if (qe == NULL)
    return -1;

  bump = &qe->data.bump_app_rx;
  bump->opaque   = sock->opaque;
  bump->rx_avail = payload_len;
  bump->rx_port = f_beui16(udp->src);
  bump->rx_ip = f_beui32(ip->src);

  ret = queue_enqueue(q, UDP_QUEUE_BUMP_APP_RX);
  if (ret != 0)
    return -1;

  return 0;
}

SEC("chamelio/event_tx")
int event_tx(struct cham_ebpf_ctx *ctx)
{
  return 0;
}

SEC("chamelio/event_deq")
int event_deq(struct cham_ebpf_ctx *ctx)
{
  return 0;
}

static __always_inline struct udp_sock *udp_sock_find(struct cham_map *maps,
    __u32 remote_ip_be, __u16 remote_port_be)
{
  struct udp_sock *sock_map;
  sock_map = maps[SOCK_MAP_IDX].addr;
  return &sock_map[0];
}

static __always_inline __u32 csum_add(__u32 sum, __u32 v)
{
  sum += v;
  return (sum & 0xffff) + (sum >> 16);
}

static __always_inline __u16 csum_fold(__u32 sum)
{
  sum = (sum & 0xffff) + (sum >> 16);
  sum = (sum & 0xffff) + (sum >> 16);
  return (__u16) ~sum;
}

static __always_inline __u32 csum_partial_be16(const void *data, __u32 len)
{
  const __u8 *p;
  __u16 w;
  __u32 sum, i;
  
  p = (const __u8 *) data;
  sum = 0;

  #pragma clang loop unroll(disable)
  for (i = 0; i + 1 < len; i += 2) 
  {
    w = ((__u16) p[i] << 8) | p[i + 1];
    sum = csum_add(sum, w);
  }
  
  if (len & 1) 
  {
    sum = csum_add(sum, (__u32) p[len - 1] << 8);
  }
  
  return sum;
}

static __always_inline __u16 ipv4_checksum(struct ip_hdr *ip)
{
  __u32 ihl_bytes = (__u32) IPH_HL(ip) << 2;
  __u32 sum = csum_partial_be16((const void *) ip, ihl_bytes);
  return csum_fold(sum);
}

static __always_inline __u16 udp_checksum(struct ip_hdr *ip,
    struct udp_hdr *udp)
{
  __u16 csum;
  __u32 udp_len, sum;
  
  udp_len = f_beui16(udp->len);
  sum = 0;

  sum = csum_add(sum, csum_partial_be16((const void *) udp, udp_len));

  sum = csum_add(sum, csum_partial_be16((const void *) &ip->src, 4));
  sum = csum_add(sum, csum_partial_be16((const void *) &ip->dst, 4));

  sum = csum_add(sum, 0x0011);                
  sum = csum_add(sum, bpf_htons(udp_len));     

  csum = csum_fold(sum);
  return csum ? csum : (__u16) 0xffff;
}
