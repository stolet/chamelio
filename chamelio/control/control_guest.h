#ifndef CONTROL_GUEST_H_
#define CONTROL_GUEST_H_

#include "control.h"
#include "queue_types.h"

void control_guest_new_proto(struct control_context *ctx,
    struct guest_control *g, struct queue_entry *qe_req);
void control_guest_new_queue(struct control_context *ctx,
    struct guest_control *g, struct queue_entry *qe_req);
void control_guest_new_map(struct control_context *ctx,
    struct guest_control *g, struct queue_entry *qe_req);
void control_guest_enableq(struct control_context *ctx,
    struct guest_control *g, struct queue_entry *qe);
void control_guest_disableq(struct control_context *ctx,
    struct guest_control *g, struct queue_entry *qe);

#endif
