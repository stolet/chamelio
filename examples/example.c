#include <cham_lib.h>

int main (int argc, char **argv)
{
  struct app_lib *a;
  // cham_init_guest();
  a = cham_init_app();
  cham_init_app_ctx(a, 0);
  while (1) {}
}