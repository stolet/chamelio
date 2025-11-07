#include <stdlib.h>
#include <linux/types.h>
#include <stdio.h>
#include <unistd.h>
#include <assert.h>
#include <sys/un.h>
#include <sys/socket.h>
#include <sys/mman.h>

#include "log.h"
#include "cham_lib.h"
#include "uxsocket.h"
#include "queue.h"

int cham_init_ivshmem()
{
  struct sockaddr_un s_un;
  int fd, ret, sock_fd;
  int64_t tmp;

  sock_fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (sock_fd < 0) 
  {
    LOG_ERROR("Failed to create socket");
    return -1;
  }

  s_un.sun_family = AF_UNIX;
  ret = snprintf(s_un.sun_path, sizeof(s_un.sun_path), 
      "%s", GUEST_SOCKET_PATH);
  if (ret < 0 || ret >= sizeof(s_un.sun_path)) 
  {
    LOG_ERROR("Could not copy unix socket path");
    goto err_close;
  }

  if (connect(sock_fd, (struct sockaddr *)&s_un, sizeof(s_un)) < 0) 
  {
    LOG_ERROR("Cannot connect to chamelio");
    goto err_close;
  }

  /* Read protocol version */
  if (uxsocket_read_one_msg(sock_fd, &tmp, &fd) < 0 || 
      (tmp != IVSHMEM_PROTOCOL_VERSION) || fd != -1) 
  {
    LOG_ERROR("Cannot read protocol version from chamelio");
    goto err_close;
  }

  /* Read guest id */
  if (uxsocket_read_one_msg(sock_fd, &tmp, &fd) < 0 || tmp < 0 || fd != -1) 
  {
    LOG_ERROR("Cannot read index and fd from chamelio");
    goto err_close;
  }

  /* now, we expect shared mem fd */
  if (uxsocket_read_one_msg(sock_fd, &tmp, &fd) < 0 || tmp != -1 || fd < 0) 
  {
    if (fd >= 0) 
      close(fd);
    
    LOG_ERROR("Cannot read shared memory fd from chamelio");
    goto err_close;
  }

  return fd;

err_close:
  close(sock_fd);
  return -1;
}