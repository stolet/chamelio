#include <stdlib.h>
#include <cham_lib.h>

int main (int argc, char **argv)
{
  int ret;
  struct guest_lib *g;
  struct proto_lib *p;
  struct proto_queue_lib *q;
  struct proto_map_lib *m;

  g = cham_connect_guest();
  if (g == NULL)
    abort();

  p = cham_new_proto(g, 0);
  if (p == NULL)
    abort();

  q = cham_new_queue(p, 16384);
  if (q == NULL)
    abort();
    
  m = cham_new_map(p, 256, 64);
  if (m == NULL)
    abort();
    
  ret = cham_enable_queue(p, q->id, 0);
  if (ret != 0)
    abort();
    
  ret = cham_disable_queue(p, m->id, 0);
  if (ret != 0)
    abort();

  while(1) {}
}