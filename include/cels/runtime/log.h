#pragma once

/**
 * @file log.h
 * @brief High-performance non-blocking ring-buffer logger for CELS.
 *
 * Provides a low-latency logging pipeline that buffers messages into a fixed-size
 * ring buffer to prevent disk/terminal I/O stutter from impacting frame-rate
 * in hot recomposition loops. Accumulated log messages are flushed to stderr/stdout
 * at safe synchronization boundaries (e.g. end of frame) via CelsFlushLogs().
 *
 * Typical usage:
 * @code
 *     // Log messages from hot composition or task loops
 *     cel_print_log(INFO, "Entity spawned at (%.2f, %.2f)", x, y);
 *     cel_print_log(WARNING, "Texture '%s' not cached", path);
 *
 *     // Flush logs at frame boundary in host runner
 *     CelsFlushLogs();
 * @endcode
 *
 * Thread safety: CelsLog is internally synchronized with a lockless ring buffer.
 * Messages can be logged concurrently from any thread.
 */

#include <stdarg.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Diagnostic log level categories.
 */
typedef enum CelsLogLevel {
    CELS_LOG_LEVEL_INFO,    /**< Informational runtime diagnostic */
    CELS_LOG_LEVEL_WARNING, /**< Non-fatal unexpected condition */
    CELS_LOG_LEVEL_ERROR    /**< Fatal or critical failure condition */
} CelsLogLevel;

/**
 * High-performance debug printer designed to avoid rendering stutter.
 *
 * Writes into a fixed-size ring buffer without blocking caller threads on I/O.
 * Call CelsFlushLogs() once per frame to output accumulated messages.
 *
 * @param level Log severity level.
 * @param file  Source filename producing the log entry. Non-NULL.
 * @param line  Source line number.
 * @param fmt   Printf-compatible format string. Non-NULL.
 */
void CelsLog(CelsLogLevel level, const char *file, int line, const char *fmt, ...);

/**
 * Convenience macro that captures __FILE__ and __LINE__ automatically.
 */
#define cel_print_log(level, ...) CelsLog(CELS_LOG_LEVEL_##level, __FILE__, __LINE__, __VA_ARGS__)

/**
 * Flushes accumulated log entries from the ring buffer to the console/terminal.
 *
 * Should be called by the host application at a safe synchronization point,
 * typically at the end of each engine tick or frame.
 */
void CelsFlushLogs(void);

#ifdef __cplusplus
}
#endif
