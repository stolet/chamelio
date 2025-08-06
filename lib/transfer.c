#include <stddef.h>

#include "queue.h"
#include "cham_lib.h"
#include "log.h"

struct buff_lib * cham_new_buf(struct app_context_lib *ctx)
{
  int ret;
  struct queue_entry *qe;
  struct queue_new_buf_req *req;
  struct buff_lib *buf;

  /* Allocate new buffer in library */
  buf = malloc(sizeof(struct buff_lib));
  if (buf == NULL)
  {
    LOG_ERROR("failed to allocate buffer");
    return NULL;
  }
  buf->head = 0;
  buf->avail = 0;

  /* Request new buffer from Chamelio */
  qe = queue_tail(ctx->app_cham_q);
  if (qe == NULL)
  {
    LOG_ERROR("app to chamelio queue is empty");
    goto free_buf;
  }

  req = (struct queue_new_buf_req *) &qe->data;
  req->opaque = (uint64_t) buf;
  ret = queue_enqueue(ctx->app_cham_q, QUEUE_NEW_BUF);
  if (ret != 0)
  {
    LOG_ERROR("failed to enqueue new buf request");
    goto free_buf;
  }

  return buf;

free_buf:
  free(buf);
  return NULL;
}

int cham_write(struct buff_lib *dst, void *src, size_t len)
{
  /* Chamelio hasn't registered buffer yet */
  if (dst->len == 0)
    return -1;

  return 0;
}

int cham_read(struct buff_lib *src, void *dst, size_t len)
{
  /* Chamelio hasn't registered buffer yet */
  if (src->len == 0)
    return -1;

  return 0;
}