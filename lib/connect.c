#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/un.h>
#include <sys/socket.h>

#include "log.h"

// TODO: Don't duplicate this
#define GUEST_SOCKET_PATH "guest_socket"
#define IVSHMEM_PROTOCOL_VERSION 0
#define HOST_PEERID 255

static int uxsocket_read_one_msg(int sock_fd, int64_t *index, int *fd);

int cham_init_guest()
{
  struct sockaddr_un s_un;
  int fd, ret, sock_fd;
  int64_t tmp;

  sock_fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (sock_fd < 0) 
  {
    LOG_ERROR("failed to create socket");
    return -1;
  }

  s_un.sun_family = AF_UNIX;
  ret = snprintf(s_un.sun_path, sizeof(s_un.sun_path), 
      "%s", GUEST_SOCKET_PATH);
  if (ret < 0 || ret >= sizeof(s_un.sun_path)) 
  {
    LOG_ERROR("could not copy unix socket path");
    goto err_close;
  }

  if (connect(sock_fd, (struct sockaddr *)&s_un, sizeof(s_un)) < 0) 
  {
    LOG_ERROR("cannot connect to chamelio");
    goto err_close;
  }

  /* Read protocol version */
  if (uxsocket_read_one_msg(sock_fd, &tmp, &fd) < 0 || 
      (tmp != IVSHMEM_PROTOCOL_VERSION) || fd != -1) 
  {
    LOG_ERROR("cannot read protocol version from chamelio");
    goto err_close;
  }

  /* Read guest id */
  if (uxsocket_read_one_msg(sock_fd, &tmp, &fd) < 0 || tmp < 0 || fd != -1) 
  {
    LOG_ERROR("cannot read index and fd from chamelio");
    goto err_close;
  }

  /* now, we expect shared mem fd + a -1 index, note that shm fd
    * is not used */
  if (uxsocket_read_one_msg(sock_fd, &tmp, &fd) < 0 || tmp != -1 || fd < 0) 
  {
    if (fd >= 0) 
      close(fd);
    
    LOG_ERROR("cannot read shared memory fd from chamelio");
    goto err_close;
  }

  return 0;

err_close:
  close(sock_fd);
  return -1;
}

static int uxsocket_read_one_msg(int sock_fd, int64_t *index, int *fd)
{
    int ret;
    struct msghdr msg;
    struct iovec iov[1];
    union {
        struct cmsghdr cmsg;
        char control[CMSG_SPACE(sizeof(int))];
    } msg_control;
    struct cmsghdr *cmsg;

    iov[0].iov_base = index;
    iov[0].iov_len = sizeof(*index);

    memset(&msg, 0, sizeof(msg));
    msg.msg_iov = iov;
    msg.msg_iovlen = 1;
    msg.msg_control = &msg_control;
    msg.msg_controllen = sizeof(msg_control);

    ret = recvmsg(sock_fd, &msg, 0);
    if (ret < sizeof(*index)) 
    {
      LOG_ERROR("cannot read message");
      perror("");
      return -1;
    }

    if (ret == 0) 
    {
      LOG_ERROR("lost connection to server");
      return -1;
    }

    // *index = GINT64_FROM_LE(*index);
    *fd = -1;

    for (cmsg = CMSG_FIRSTHDR(&msg); cmsg; cmsg = CMSG_NXTHDR(&msg, cmsg)) 
    {

      if (cmsg->cmsg_len != CMSG_LEN(sizeof(int)) ||
          cmsg->cmsg_level != SOL_SOCKET ||
          cmsg->cmsg_type != SCM_RIGHTS) 
      {
        continue;
      }

      memcpy(fd, CMSG_DATA(cmsg), sizeof(*fd));
    }

    return 0;
}