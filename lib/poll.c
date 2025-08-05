#include "log.h"
#include "queue.h"
#include "cham_lib.h"

static int handle_new_buf(struct queue_entry *qe);
static int handle_bump(struct queue_entry *qe);

int cham_poll_slow(struct app_context_lib *actx)
{
  struct queue_entry *qe;
  qe = queue_head(actx->cham_app_q);

  switch (qe->type)
  {
    case QUEUE_NEW_BUF:
      handle_new_buf(qe);
      break;
    default:
      LOG_ERROR("unknown queue entry type=%d", qe->type);
      return -1;
  }

  return 0;
}

int cham_poll_bump(struct app_context_lib *actx)
{
  int i;
  struct queue_entry *qe;

  for (i = 0; i < actx->n_fp_cores; i++)
  {
    qe = queue_head(actx->bump_app_q[i]);
    if (qe == NULL)
      continue;

    switch (qe->type)
    {
      case QUEUE_BUMP:
        handle_bump(qe);
        break;
      default:
        LOG_ERROR("unknown queue entry type=%d", qe->type);
        return -1;
    }
  }

  return 0;
}

static int handle_new_buf(struct queue_entry *qe)
{
  struct buff_lib *buf;
  struct queue_new_buf_res *res;
  
  res = (struct queue_new_buf_res *) &qe->data;
  buf = (struct buff_lib *) res->opaque;
  buf->base = res->base;
  buf->len = res->len;

  LOG_DEBUG("got base for new buffer");

  return 0;
}

static int handle_bump(struct queue_entry *qe)
{
  uint64_t head;
  struct buff_lib *buf;
  struct queue_buf_bump *bump;
  
  bump = (struct queue_buf_bump *) &qe->data;
  buf = (struct buff_lib *) bump->opaque;
  
  head = buf->head + bump->head_bump;
  if (head > buf->len)
    head = head - buf->len;

  buf->head = head;
  buf->avail += bump->avail_bump;

  return 0;
}