#ifndef LOG_H_
#define LOG_H_

#define LOG_DEBUG(...) log_debug(__FILE__, __LINE__, __func__, __VA_ARGS__)

#define LOG_INFO(...) log_info(__FILE__, __LINE__, __func__, __VA_ARGS__)

#define LOG_WARN(...) log_warn(__FILE__, __LINE__, __func__, __VA_ARGS__)

#define LOG_ERROR(...) log_error(__FILE__, __LINE__, __func__, __VA_ARGS__)

extern void log_debug(const char *file, int line, 
  const char *func, const char *fmt, ...);

extern void log_info(const char *file, int line, 
  const char *func, const char *fmt, ...);

extern void log_warn(const char *file, int line, 
  const char *func, const char *fmt, ...);

extern void log_error(const char *file, int line, 
  const char *func, const char *fmt, ...);

#endif