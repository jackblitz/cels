#include "cels/log.h"
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
