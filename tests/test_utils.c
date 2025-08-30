#include "test_utils.h"

/* Global variable definition */
pid_t g_chamelio_pid = -1;

void cleanup_handler(int signo)
{
  if (g_chamelio_pid > 0)
  {
    kill(g_chamelio_pid, SIGTERM);
    waitpid(g_chamelio_pid, NULL, 0);
  }
  printf(ANSI_COLOR_RESET);
  fflush(stdout);
  exit(1);
}
