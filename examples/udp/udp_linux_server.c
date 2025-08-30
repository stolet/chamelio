/* Usage:
 *   ./udp_echo_server <bind_ip> <port> [--buf-size N]
 *
 * Example:
 *   ./udp_echo_server 0.0.0.0 9000 --buf-size 2048
 */

#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

int main(int argc, char **argv)
{
  if (argc < 3)
  {
    fprintf(stderr, "Usage: %s <bind_ip> <port> [--buf-size N]\n", argv[0]);
    return EXIT_FAILURE;
  }

  const char *bind_ip = argv[1];
  int port = atoi(argv[2]);
  if (port <= 0 || port > 65535)
  {
    fprintf(stderr, "Invalid port\n");
    return EXIT_FAILURE;
  }

  size_t buf_size = 65536;

  for (int i = 3; i < argc; i++)
  {
    if (strcmp(argv[i], "--buf-size") == 0 && i + 1 < argc)
    {
      buf_size = (size_t)strtoul(argv[++i], NULL, 10);
    }
    else
    {
      fprintf(stderr, "Unknown arg: %s\n", argv[i]);
      return EXIT_FAILURE;
    }
  }

  int fd = socket(AF_INET, SOCK_DGRAM, 0);
  if (fd < 0) 
  {
    perror("socket");
    return EXIT_FAILURE;
  }

  struct sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_port   = htons((uint16_t)port);
  if (inet_pton(AF_INET, bind_ip, &addr.sin_addr) != 1)
  {
    fprintf(stderr, "Invalid bind_ip\n");
    return EXIT_FAILURE;
  }

  if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0)
  {
    perror("bind");
    return EXIT_FAILURE;
  }

  uint8_t *buf = (uint8_t *)malloc(buf_size);
  if (!buf) 
  {
    perror("malloc");
    return EXIT_FAILURE;
  }

  printf("UDP echo server listening on %s:%d (buf-size=%zu)\n", 
      bind_ip, port, buf_size);
  fflush(stdout);
  
  
  for (;;)
  {
    struct sockaddr_in src;
    socklen_t srclen = sizeof(src);
    ssize_t n = recvfrom(fd, buf, buf_size, 0, (struct sockaddr *)&src, &srclen);
    if (n < 0)
    {
      if (errno == EINTR) continue;
      perror("recvfrom");
      return EXIT_FAILURE;
    }

    ssize_t m = sendto(fd, buf, (size_t)n, 0, (struct sockaddr *)&src, srclen);
    if (m < 0)
    {
      if (errno == EINTR) 
        continue;
    }
  }

  free(buf);
  close(fd);
  return 0;
}
