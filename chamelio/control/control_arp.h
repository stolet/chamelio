#ifndef CONTROL_ARP_H_
#define CONTROL_ARP_H_

#include "control.h"
#include "queue_types.h"

void control_arp_lookup(struct control_context *ctx,
    struct queue_entry *qe);
void control_arp_mbuf(struct control_context *ctx,
    struct queue_entry *qe, __u16 core);
void control_arp_req(struct control_context *ctx,
    struct queue_entry *qe);
void control_arp_rep(struct control_context *ctx,
    struct queue_entry *qe);
void control_arp_timeout(struct control_context *ctx,
    struct to_entry *te);

#endif
