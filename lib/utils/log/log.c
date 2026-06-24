#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>

#include "log.h"

#define LOG_MAX_LEN 256

int cham_log_level = CHAM_LOG_LEVEL_INFO;

__attribute__((constructor))
static void log_level_init(void)
{
  const char *env = getenv("CHAM_DEBUG");
  if (env != NULL && env[0] != '\0' && env[0] != '0')
    cham_log_level = CHAM_LOG_LEVEL_DEBUG;
}

void log_set_level(int level)
{
  cham_log_level = level;
}

void log_debug(const char *file, int line,
  const char *func, const char *fmt, ...)
{
  va_list args;
  char msg[LOG_MAX_LEN];
 
  va_start(args, fmt);
  vsnprintf(msg, LOG_MAX_LEN, fmt, args);
  va_end(args);
 
  fprintf(stderr, "DEBUG: [%s:%d] %s(): %s\n", file, line, func, msg);
}

void log_info(const char *file, int line, const char *func, const char *fmt, ...)
{
  va_list args;
  char msg[LOG_MAX_LEN];
 
  va_start(args, fmt);
  vsnprintf(msg, LOG_MAX_LEN, fmt, args);
  va_end(args);
 
  fprintf(stderr, "INFO: [%s:%d] %s(): %s\n", file, line, func, msg);
}

void log_info_plain(const char *fmt, ...)
{
  va_list args;
  char msg[LOG_MAX_LEN];

  va_start(args, fmt);
  vsnprintf(msg, LOG_MAX_LEN, fmt, args);
  va_end(args);

  fprintf(stderr, "INFO: %s\n", msg);
}

void log_warn(const char *file, int line, const char *func, const char *fmt, ...)
{
  va_list args;
  char msg[LOG_MAX_LEN];
 
  va_start(args, fmt);
  vsnprintf(msg, LOG_MAX_LEN, fmt, args);
  va_end(args);
 
  fprintf(stderr, "WARN: [%s:%d] %s(): %s\n", file, line, func, msg);
}

void log_error(const char *file, int line, const char *func, const char *fmt, ...)
{
  va_list args;
  char msg[LOG_MAX_LEN];
 
  va_start(args, fmt);
  vsnprintf(msg, LOG_MAX_LEN, fmt, args);
  va_end(args);
 
  fprintf(stderr, "ERROR: [%s:%d] %s(): %s\n", file, line, func, msg);
}
