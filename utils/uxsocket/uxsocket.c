#include <stdint.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <string.h>

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