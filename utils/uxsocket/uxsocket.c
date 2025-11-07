#include <linux/types.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <string.h>
#include <stdio.h>

#include "log.h"

int uxsocket_send_int(int fd, int64_t i)
{
  int n;

  struct iovec iov = {
      .iov_base = &i,
      .iov_len = sizeof(i),
  };

  struct msghdr msg = {
      .msg_name = NULL,
      .msg_namelen = 0,
      .msg_iov = &iov,
      .msg_iovlen = 1,
      .msg_control = NULL,
      .msg_controllen = 0,
      .msg_flags = 0,
  };

  if ((n = sendmsg(fd, &msg, 0)) != sizeof(int64_t))
  {
      LOG_ERROR("failed to send msg");
      return -1;
  }

  return n;
}

int uxsocket_sendfd(int uxfd, int fd, int64_t i)
{
  int n;
  struct cmsghdr *chdr;

  /* Need to pass at least one byte of data to send control data */
  struct iovec iov = {
      .iov_base = &i,
      .iov_len = sizeof(i),
  };

  /* Allocate a char array but use a union to ensure that it
      is alligned properly */
  union
  {
      char buf[CMSG_SPACE(sizeof(fd))];
      struct cmsghdr align;
  } cmsg;
  memset(&cmsg, 0, sizeof(cmsg));

  /* Add control data (file descriptor) to msg */
  struct msghdr msg = {
      .msg_name = NULL,
      .msg_namelen = 0,
      .msg_iov = &iov,
      .msg_iovlen = 1,
      .msg_control = &cmsg,
      .msg_controllen = sizeof(cmsg),
      .msg_flags = 0,
  };

  /* Set message header to describe ancillary data */
  chdr = CMSG_FIRSTHDR(&msg);
  chdr->cmsg_level = SOL_SOCKET;
  chdr->cmsg_type = SCM_RIGHTS;
  chdr->cmsg_len = CMSG_LEN(sizeof(int));
  memcpy(CMSG_DATA(chdr), &fd, sizeof(fd));

  if ((n = sendmsg(uxfd, &msg, 0)) != sizeof(i))
  {
    LOG_ERROR("failed to send message");
    return -1;
  }

  return n;
}

int uxsocket_read_one_msg(int sock_fd, int64_t *index, int *fd)
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