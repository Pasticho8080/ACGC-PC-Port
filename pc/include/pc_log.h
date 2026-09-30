#ifndef PC_LOG_H
#define PC_LOG_H

#include <stdarg.h>
#include <stdint.h>

#include "types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* How many recent log lines get embedded in a crash record. */
#define AC_LOG_RING_LINES 40

/** Opens logs/session-<timestamp>.log. Safe to call once at startup. Never fails hard. */
extern void pc_log_init(void);

/**
 * Appends a line to the session log (and the console when AC_DEBUG_LOG).
 * A no-op macro without the flag, so tracing costs nothing in release.
 */
#ifdef AC_DEBUG_LOG
extern void pc_logf(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
extern void pc_vlogf(const char* fmt, va_list args);
#define PC_LOG(...) pc_logf(__VA_ARGS__)
#else
#define PC_LOG(...) ((void)0)
#endif

/** Writes the crash record for an OSPanic: the panic message, the game state
 * (scene, frame, doing_point, arena) and the tail of the session log.
 * Returns the path written, or NULL if it could not be saved. */
extern const char* pc_log_crash(const char* file, int line, const char* msg, va_list args);

/** Same record for a hardware fault (SIGSEGV/SIGILL/SIGFPE), written from the
 * crash handler on the way down so a fault is as reportable as a panic. */
extern const char* pc_log_fault(const char* kind, uintptr_t addr, uintptr_t data_addr);

/** Shows a modal dialog naming the crash log, so it can be re-shown without
 * writing a second record. */
extern void pc_log_show_crash_dialog(const char* log_path, const char* file, int line);

/** Absolute path of the active session log, or NULL if none is open. */
extern const char* pc_log_session_path(void);

#ifdef __cplusplus
}
#endif

#endif /* PC_LOG_H */