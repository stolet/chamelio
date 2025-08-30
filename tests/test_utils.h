#ifndef TEST_UTILS_H
#define TEST_UTILS_H

#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <unistd.h>
#include <sys/wait.h>

/* ANSI color codes */
#define ANSI_COLOR_BLUE    "\x1b[34m"
#define ANSI_COLOR_GREEN   "\x1b[32m"
#define ANSI_COLOR_RED     "\x1b[31m"
#define ANSI_COLOR_RESET   "\x1b[0m"

/* Global variable to store process PIDs for cleanup */
extern pid_t g_chamelio_pid;

/* Custom assert macro with colored output */
#define TEST_ASSERT(condition, message) \
  do { \
    if (!(condition)) { \
      printf(ANSI_COLOR_RED "FAILED: %s" ANSI_COLOR_RESET "\n", message); \
      if (g_chamelio_pid > 0) { \
        kill(g_chamelio_pid, SIGTERM); \
        waitpid(g_chamelio_pid, NULL, 0); \
      } \
      exit(1); \
    } \
  } while (0)

/* Signal handler for cleanup */
void cleanup_handler(int signo);

#endif /* TEST_UTILS_H */
