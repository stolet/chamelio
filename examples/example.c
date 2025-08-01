#include <cham_lib.h>

int main (int argc, char **argv)
{
  // cham_init_guest();
  cham_init_app();
  cham_init_app_ctx(0);
  while (1) {}
}