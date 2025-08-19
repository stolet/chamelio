#include <stdlib.h>
#include <assert.h>

#include <cham_lib.h>
#include <udp_lib.h>

int main (int argc, char **argv)
{
  struct guest_lib *g;
  struct proto_lib *p;

  int ret;

  ret = udp_connect_slow();
  if (ret != 0)
    abort();
    
  ret = udp_ctx_new();
  if (ret != 0)
    abort();

  // g = cham_connect_guest();
  // assert(g != NULL);

  // p = cham_new_proto(g, 8192);
  // assert(p != NULL);
}