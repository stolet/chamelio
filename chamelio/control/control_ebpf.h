#ifndef CONTROL_EBPF_H_
#define CONTROL_EBPF_H_

#include "control.h"
#include "queue_types.h"

void control_ebpf_allocate(struct guest_control *g,
    struct queue_entry *qe_req);
void control_ebpf_free(struct guest_control *g,
    struct queue_entry *qe_req);
void control_ebpf_upload(struct control_context *ctx,
    struct guest_control *g, struct queue_entry *qe_req);
int control_ebpf_init(struct control_context *ctx);
int control_ebpf_publish(struct control_context *ctx);

#endif
