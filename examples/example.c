#include <stdlib.h>
#include <cham_lib.h>

int main (int argc, char **argv)
{
  int ret;
  struct guest_lib *g;
  struct proto_lib *p;

  g = cham_connect_guest();
  if (g == NULL)
    abort();

  p = cham_new_proto(g, 0);
  if (p == NULL)
    abort();

  ret = cham_new_queue(p, 16384);
  if (ret != 0)
    abort();
    
  ret = cham_new_map(p, 256, 64);
  if (ret != 0)
    abort();
    
  ret = cham_enable_queue(p, 6, 0);
  if (ret != 0)
    abort();
    
  ret = cham_disable_queue(p, 6, 0);
  if (ret != 0)
    abort();

  while(1) {}
}