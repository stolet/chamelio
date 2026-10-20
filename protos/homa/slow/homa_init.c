#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "homa_slow.h"
#include "queue_fns.h"

static void *map_new(struct proto_lib *proto, __u32 nelems, __u32 elsize);
static int upload_fast_path(struct proto_lib *proto);

int homa_init_context(struct homa_slow *ctx)
{
  struct guest_lib *guest;
  void *rpcs;

  ctx->listen_fd = -1;
  ctx->app_fd = -1;

  if (ctx->config.virt)
  {
    ctx->proto = cham_new_proto_virt(CHAM_PROTO_HOMA);
  }
  else
  {
    guest = cham_connect_guest();
    if (guest == NULL)
      return -1;

    ctx->proto = cham_new_proto_bare(guest, CHAM_PROTO_HOMA);
  }

  if (ctx->proto == NULL)
    return -1;

  if (ctx->proto->n_fp_cores != 1)
  {
    fprintf(stderr, "Homa requires one fast-path core\n");
    errno = EOPNOTSUPP;
    return -1;
  }

  ctx->ports = map_new(ctx->proto, UINT16_MAX + 1U, sizeof(*ctx->ports));
  if (ctx->ports == NULL)
    return -1;

  ctx->sockets = map_new(ctx->proto, HOMA_MAX_SOCKETS, sizeof(*ctx->sockets));
  if (ctx->sockets == NULL)
    return -1;

  rpcs = map_new(ctx->proto, HOMA_MAX_RPCS, sizeof(struct homa_rpc));
  if (rpcs == NULL)
    return -1;

  if (map_new(ctx->proto, 1, sizeof(struct homa_state)) == NULL)
    return -1;

  ctx->info.sockets_off = (char *) ctx->sockets - (char *) ctx->proto->shm_base;
  ctx->info.rpcs_off = (char *) rpcs - (char *) ctx->proto->shm_base;
  ctx->info.shm_len = ctx->proto->shm_size;
  ctx->info.shm_off = ctx->proto->shm_off;

  return upload_fast_path(ctx->proto);
}

static void *map_new(struct proto_lib *proto, __u32 nelems, __u32 elsize)
{
  struct proto_map_lib *map;
  void *addr;

  map = cham_new_map(proto, nelems, elsize);
  if (map == NULL)
    return NULL;

  addr = (char *) proto->shm_base + map->off;
  memset(addr, 0, (size_t) nelems * elsize);
  return addr;
}

static int upload_fast_path(struct proto_lib *proto)
{
  struct stat st;
  void *bytecode;
  int fd;
  int ret;

  fd = open(HOMA_EBPF_PATH, O_RDONLY | O_CLOEXEC);
  if (fd < 0)
    return -1;

  if (fstat(fd, &st) != 0 || st.st_size <= 0 || st.st_size > UINT32_MAX)
  {
    close(fd);
    errno = EINVAL;
    return -1;
  }

  bytecode = mmap(NULL, st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
  close(fd);

  if (bytecode == MAP_FAILED)
    return -1;

  ret = -1;

  if (cham_allocate_ebpf(proto, st.st_size) != NULL)
    ret = cham_upload_ebpf(proto, bytecode, st.st_size);

  munmap(bytecode, st.st_size);
  return ret;
}
