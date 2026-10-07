#include "cels/runtime/log.h"
#include <stdio.h>
#include <string.h>

#define CELS_LOG_BUFFER_SIZE 256
#define CELS_LOG_MAX_LENGTH 512

typedef struct {
    CelsLogLevel level;
    char message[CELS_LOG_MAX_LENGTH];
} CelsLogMessage;

static CelsLogMessage s_logBuffer[CELS_LOG_BUFFER_SIZE];
static volatile int s_logHead = 0; // write index
static volatile int s_logTail = 0; // read index
static volatile int s_logDropped = 0;

/**
 * Formats and enqueues a log message into the internal fixed ring buffer.
 *
 * Extracts the file basename, prepends a timestamped/level severity prefix,
 * formats the variable arguments into a fixed-size buffer, and advances the
 * ring buffer head. If the buffer is full, the message is dropped and a dropped
 * counter is incremented to prevent I/O blocking during active frame execution.
 *
 * @param level Log severity level (CELS_LOG_LEVEL_INFO, WARNING, ERROR).
 * @param file  Source file path where the log originated (basename extracted).
 * @param line  Source line number.
 * @param fmt   Printf-style format string. Non-NULL.
 * @param ...   Format string arguments.
 */
void CelsLog(CelsLogLevel level, const char *file, int line, const char *fmt, ...)
{
    int nextHead = (s_logHead + 1) % CELS_LOG_BUFFER_SIZE;
    if (nextHead == s_logTail) {
        // Buffer full, drop message
        s_logDropped++;
        return;
    }

    CelsLogMessage *msg = &s_logBuffer[s_logHead];
    msg->level = level;
    
    char prefix[64];
    const char *levelStr = "INFO";
    if (level == CELS_LOG_LEVEL_WARNING) levelStr = "WARN";
    else if (level == CELS_LOG_LEVEL_ERROR) levelStr = "ERROR";
    
    // Just file basename
    const char *base = strrchr(file, '/');
    if (!base) {
        base = strrchr(file, '\\');
    }
    base = base ? base + 1 : file;

    snprintf(prefix, sizeof(prefix), "[%s] %s:%d: ", levelStr, base, line);
    
    size_t prefixLen = strlen(prefix);
    strcpy(msg->message, prefix);
    
    va_list args;
    va_start(args, fmt);
    vsnprintf(msg->message + prefixLen, CELS_LOG_MAX_LENGTH - prefixLen, fmt, args);
    va_end(args);
    
    s_logHead = nextHead;
}

/**
 * Flushes all pending log messages from the ring buffer to standard output streams.
 *
 * Drains messages from tail to head, routing ERROR level logs to stderr and other
 * levels to stdout. If any messages were dropped due to ring buffer overflow, a
 * warning notice is emitted to stderr. Finally, flushes both stdout and stderr.
 */
void CelsFlushLogs(void)
{
    if (s_logHead == s_logTail && s_logDropped == 0) return;
    
    // Batch print to avoid multiple I/O calls
    while (s_logTail != s_logHead) {
        CelsLogMessage *msg = &s_logBuffer[s_logTail];
        FILE *out = (msg->level == CELS_LOG_LEVEL_ERROR) ? stderr : stdout;
        fprintf(out, "%s\n", msg->message);
        s_logTail = (s_logTail + 1) % CELS_LOG_BUFFER_SIZE;
    }
    
    if (s_logDropped > 0) {
        fprintf(stderr, "[CELS WARN] %d log messages were dropped to prevent stuttering.\n", s_logDropped);
        s_logDropped = 0;
    }
    
    fflush(stdout);
    fflush(stderr);
}
