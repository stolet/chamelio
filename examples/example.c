#include <stdlib.h>
#include <cham_lib.h>

int main (int argc, char **argv)
{
  struct guest_lib *g;
  struct proto_lib *p;

struct guest_lib * cham_connect_guest();
/* Creates a new protocol and maps shared memory region */
struct proto_lib* cham_new_proto(struct guest_lib *g, uint32_t shmsize);

  g = cham_connect_guest();
  if (g == NULL)
    abort();

  p = cham_new_proto(g, 0);
  if (p == NULL)
    abort();
}