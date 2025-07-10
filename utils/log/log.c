#include <stdio.h>
#include <stdarg.h>

#define LOG_MAX_LEN 256

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

void log_warn(const char *file, int line, const char *func, const char *fmt, ...)
{
  va_list args;
  char msg[LOG_MAX_LEN];
 
  va_start(args, fmt);
  vsnprintf(msg, LOG_MAX_LEN, fmt, args);
  va_end(args);
 
  fprintf(stderr, "INFO: [%s:%d] %s(): %s\n", file, line, func, msg);
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