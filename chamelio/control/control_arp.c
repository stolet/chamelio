#include <linux/types.h>
#include <string.h>

#include "control_arp.h"
#include "arp.h"
#include "clock.h"
#include "log.h"
#include "nic.h"
#include "queue_fns.h"

static int update_arp_fast(struct control_context *ctx,
    __u32 ip, const __u8 *mac);
static int arp_reply_targets_local(struct control_context *ctx,
    const struct queue_arp_rx_rep *arp_rep);
static int mac_is_broadcast(const __u8 *mac);

void control_arp_timeout(struct control_context *ctx,
    struct to_entry *te)
{
  int ret;
  struct arp_table_entry *ae = (struct arp_table_entry *)te->data;

  /* If this entry is not pending anymore return */
  if (!ae->pending)
    return;

  /* Send another ARP request */
  ret = arp_request(ctx->txqs[0], ctx->ctl_fast_qs[0], ae->ip,
        (__u8 *)&ctx->nic_ctx->eth_addr.addr_bytes, ctx->config->ip);
  if (ret != 0)
  {
    LOG_ERROR("failed to enqueue ARP request");
    return;
  }

  /* Enqueue a new timeout */
  te = tomgr_insert(ctx->tomgr, TO_ARP, clock_tsc_after_us(ARP_TIMEOUT), ae);
  if (te == NULL)
  {
    LOG_ERROR("failed to insert ARP timeout");
    return;
  }
  ae->te = te;
}

void control_arp_lookup(struct control_context *ctx,
    struct queue_entry *qe)
{
  int ret;
  struct arp_table_entry *ae;
  struct to_entry *te;
  struct queue_arp_lookup *le;

  le = &qe->data.arp_lookup;

  /* Ignore lookup if this address is resolved or pending */
  ae = arp_lookup(&ctx->arp_table, le->ip);

  if (ae == NULL)
  {
    /* Insert as pending to ARP table */
    ae = arp_insert_pending(&ctx->arp_table, le->ip);

    /* Enqueue ARP request to fast-path */
    ret = arp_request(ctx->txqs[0], ctx->ctl_fast_qs[0], le->ip,
        (__u8 *)&ctx->nic_ctx->eth_addr.addr_bytes, ctx->config->ip);
    if (ret != 0)
    {
      LOG_ERROR("failed to enqueue ARP request");
      return;
    }

    /* Insert timeout for ARP request */
    te = tomgr_insert(ctx->tomgr, TO_ARP, clock_tsc_after_us(ARP_TIMEOUT), ae);
    if (te == NULL)
    {
      LOG_ERROR("failed to insert timeout for ARP request");
      return;
    }
    ae->te = te;
  }
}

void control_arp_mbuf(struct control_context *ctx,
    struct queue_entry *qe, __u16 core)
{
  int ret;
  struct arp_table_entry *ae;
  struct queue_entry *txqe;
  struct queue_mbuf_pending *mbe;

  mbe = &qe->data.mbuf_pending;

  ae = arp_lookup(&ctx->arp_table, mbe->ip);
  if (ae != NULL && !ae->pending)
  {
    txqe = queue_tail(ctx->ctl_fast_qs[core]);
    if (txqe == NULL)
    {
      LOG_ERROR("failed to get tail of control->fast queue");
      return;
    }

    txqe->data.mbuf_tx.gid = mbe->gid;
    txqe->data.mbuf_tx.pkt_len = mbe->pkt_len;
    txqe->data.mbuf_tx.mb = mbe->mb;

    ret = queue_enqueue(ctx->ctl_fast_qs[core], QUEUE_MBUF_TX);
    if (ret != 0)
    {
      LOG_ERROR("failed to enqueue mbuf to control->fast queue");
      return;
    }

    return;
  }

  /* Insert pending mbuf to ARP buffer */
  ret = arp_buf_insert(&ctx->arp_buf, mbe->ip, core,
      mbe->gid, mbe->pkt_len, mbe->mb);
  if (ret < 0)
  {
    LOG_ERROR("buf for mbufs waiting for arp resolution is full");
  }
}

void control_arp_req(struct control_context *ctx,
  struct queue_entry *qe)
{
  int ret;
  struct arp_table_entry *ae;
  struct queue_arp_rx_req *arp_req = &qe->data.arp_pkt_rx_req;

  /* Check if ARP request was for me*/
  if (arp_req->tpa != ctx->config->ip)
    return;

  ae = arp_lookup(&ctx->arp_table, arp_req->spa);
  if (ae == NULL || ae->pending)
  {
    /* Add sender to ARP table */
    if (arp_insert(&ctx->arp_table, arp_req->spa,
        (__u8 *)&arp_req->sha) == NULL)
    {
      LOG_ERROR("failed to add sender to ARP table");
      return;
    }

    ret = update_arp_fast(ctx, arp_req->spa, (__u8 *)&arp_req->sha);
    if (ret != 0)
      return;

    /* Flush all packets pending on this ARP resolution */
    arp_buf_flush(&ctx->arp_buf, arp_req->spa, ctx->ctl_fast_qs);
  }

  /* Enqueue ARP reply for fast-path */
  ret = arp_reply(ctx->txqs[0], ctx->ctl_fast_qs[0],
      (__u8 *)&ctx->nic_ctx->eth_addr.addr_bytes,
      ctx->config->ip, (__u8 *)&arp_req->sha, arp_req->spa);
  if (ret != 0)
  {
    LOG_ERROR("failed to enqueue ARP reply");
    return;
  }
}

void control_arp_rep(struct control_context *ctx,
  struct queue_entry *qe)
{
  int ret;
  struct arp_table_entry *ae;
  struct queue_arp_rx_rep *arp_rep = &qe->data.arp_pkt_rx_rep;

  /* Check if this ARP reply is for us and is pending */
  if (!arp_reply_targets_local(ctx, arp_rep))
  {
    LOG_WARN("dropping ARP reply not addressed to local host spa=%x tpa=%x",
        arp_rep->spa, arp_rep->tpa);
    return;
  }

  ae = arp_lookup(&ctx->arp_table, arp_rep->spa);
  if (ae == NULL || !ae->pending)
    return;

  ae = arp_insert(&ctx->arp_table, arp_rep->spa, (__u8 *)&arp_rep->sha);
  if (ae == NULL)
    LOG_ERROR("ARP table full");

  /* Cancel timeout */
  if (ae->te != NULL)
  {
    tomgr_cancel(ctx->tomgr, ae->te);
    ae->te = NULL;
  }

  ret = update_arp_fast(ctx, arp_rep->spa, (__u8 *)&arp_rep->sha);
  if (ret != 0)
    return;

  /* Flush all packets pending on this ARP resolution */
  arp_buf_flush(&ctx->arp_buf, ae->ip, ctx->ctl_fast_qs);
}

static int update_arp_fast(struct control_context *ctx,
    __u32 ip, const __u8 *mac)
{
  int ret, i;
  struct queue_entry *arp_up;

  for (i = 0; i < ctx->config->fp_cores_max; i++)
  {
    arp_up = queue_tail(ctx->ctl_fast_qs[i]);
    if (arp_up == NULL)
    {
      LOG_ERROR("failed to get tail of control->fast queue");
      return -1;
    }

    arp_up->data.arp_update.ip = ip;
    memcpy(&arp_up->data.arp_update.mac, mac, ETH_ADDR_LEN);

    ret = queue_enqueue(ctx->ctl_fast_qs[i], QUEUE_ARP_UPDATE);
    if (ret != 0)
    {
      LOG_ERROR("failed to enqueue ARP update to control->fast queue");
      return ret;
    }
  }

  return 0;
}

static int arp_reply_targets_local(struct control_context *ctx,
    const struct queue_arp_rx_rep *arp_rep)
{
  const __u8 *local_mac;
  const __u8 *target_mac;

  if (arp_rep->tpa != ctx->config->ip)
    return 0;

  local_mac = ctx->nic_ctx->eth_addr.addr_bytes;
  target_mac = (const __u8 *) &arp_rep->tha;

  return memcmp(target_mac, local_mac, ETH_ADDR_LEN) == 0 ||
      mac_is_broadcast(target_mac);
}

static int mac_is_broadcast(const __u8 *mac)
{
  int i;

  for (i = 0; i < ETH_ADDR_LEN; i++)
  {
    if (mac[i] != 0xff)
      return 0;
  }

  return 1;
}
