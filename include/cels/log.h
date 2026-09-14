#ifndef CELS_LOG_H
#define CELS_LOG_H

#include <stdarg.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    CELS_LOG_LEVEL_INFO,
    CELS_LOG_LEVEL_WARNING,
    CELS_LOG_LEVEL_ERROR
} CelsLogLevel;

/**
 * High-performance debug printer designed to avoid rendering stutter.
 * It uses a fixed-size ring buffer so printing from hot loops won't block the main thread.
 * Call CelsFlushLogs() once per frame to output the accumulated messages.
 */
void CelsLog(CelsLogLevel level, const char *file, int line, const char *fmt, ...);

#define cel_print_log(level, ...) CelsLog(CELS_LOG_LEVEL_##level, __FILE__, __LINE__, __VA_ARGS__)

/**
 * Flush accumulated logs to the console. Should be called by the host application
 * (e.g., inside the main engine loop) at a safe time, like the end of the frame.
 */
void CelsFlushLogs(void);

#ifdef __cplusplus
}
#endif

#endif // CELS_LOG_H
