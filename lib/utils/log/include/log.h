#ifndef LOG_H_
#define LOG_H_

#define CHAM_LOG_LEVEL_INFO  0
#define CHAM_LOG_LEVEL_DEBUG 1

/* Gate LOG_DEBUG on the runtime level so it is zero-cost when disabled. */
#define LOG_DEBUG(...) do { \
  if (cham_log_level >= CHAM_LOG_LEVEL_DEBUG) \
    log_debug(__FILE__, __LINE__, __func__, __VA_ARGS__); \
} while (0)

#define LOG_INFO(...) log_info(__FILE__, __LINE__, __func__, __VA_ARGS__)

#define LOG_INFO_PLAIN(...) log_info_plain(__VA_ARGS__)

#define LOG_WARN(...) log_warn(__FILE__, __LINE__, __func__, __VA_ARGS__)

#define LOG_ERROR(...) log_error(__FILE__, __LINE__, __func__, __VA_ARGS__)

#ifdef __cplusplus
extern "C" {
#endif

/* Current log level; set via log_set_level() */
extern int cham_log_level;

extern void log_set_level(int level);

extern void log_debug(const char *file, int line,
  const char *func, const char *fmt, ...);

extern void log_info(const char *file, int line,
  const char *func, const char *fmt, ...);

extern void log_info_plain(const char *fmt, ...);

extern void log_warn(const char *file, int line,
  const char *func, const char *fmt, ...);

extern void log_error(const char *file, int line,
  const char *func, const char *fmt, ...);

#ifdef __cplusplus
}
#endif

#endif
