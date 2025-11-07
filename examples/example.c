#include <stdlib.h>
#include <arpa/inet.h>
#include <string.h>
#include <sys/socket.h>

#include <udp_lib.h>

int main (int argc, char **argv)
{
  int ret, sockfd;
  struct sockaddr_in addr;
  socklen_t addr_len;
  __u8 buf[2048];
  // struct proto_lib *p;
  // struct guest_lib *g;
  
  ret = udp_connect_slow();
  if (ret != 0)
    abort();
    
  ret = udp_ctx_new();
  if (ret != 0)
    abort();

  sockfd = udp_socket();
  if (sockfd < 0)
    abort();

  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  inet_pton(AF_INET, "192.168.10.13", &(addr.sin_addr));
  addr.sin_port = htons(1234);
  addr_len = sizeof(addr);
  
  // ret = udp_sendto(sockfd, buf, sizeof(buf), 
  //     (struct sockaddr *) &addr, addr_len);
  // if (ret < 0)
  //   abort();

  while(1){
     udp_sendto(sockfd, buf, sizeof(buf), 
       (struct sockaddr *) &addr, addr_len);
  }
  // g = cham_connect_guest();
  // assert(g != NULL);

  // p = cham_new_proto(g, 8192);
  // assert(p != NULL);
}