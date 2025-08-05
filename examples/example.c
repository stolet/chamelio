#include <stdlib.h>
#include <cham_lib.h>

int main (int argc, char **argv)
{
  struct app_lib *a;
  struct app_context_lib *actx;
  struct buff_lib *buf;

  // cham_init_guest();
  a = cham_init_app();
  actx = cham_init_app_ctx(a, 0);
  buf = cham_new_buf(actx);
  if (buf == NULL)
  {
    abort();
  }

  while (1) {
    cham_poll_slow(actx);
    cham_poll_bump(actx);
  }
}