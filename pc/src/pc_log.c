/* pc_log.c - crash records and optional verbose session logging.
 * Crash records are always compiled in; the session log needs AC_DEBUG_LOG. */

#include "pc_log.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#include <direct.h>
#include <windows.h>
#define AC_MKDIR(p) _mkdir(p)
#else
#include <sys/stat.h>
#include <sys/types.h>
#define AC_MKDIR(p) mkdir((p), 0775)
#endif

#include "SDL.h"

/* Decomp state we want to capture at crash time. */
#include "game.h"
#include "graph.h"
#include "m_common_data.h"
#include "m_malloc.h"

#include "pc_platform.h"

#define AC_LOG_MAX_PATH 1024
#define AC_LOG_LINE_LEN 256

static char g_log_dir[AC_LOG_MAX_PATH];
static int g_log_dir_ready = 0;

#ifdef AC_DEBUG_LOG
static FILE* g_session = NULL;
static char g_session_path[AC_LOG_MAX_PATH];
static int g_session_ready = 0;

/* Ring buffer of recent lines, embedded in the crash record so the record
 * carries the context of what the game was doing when it died. */
static char g_ring[AC_LOG_RING_LINES][AC_LOG_LINE_LEN];
static int g_ring_next = 0;
static int g_ring_count = 0;
#endif

/* OSPanic is reachable from several places and used to be re-entered once per
 * frame; a second record or a second dialog is worse than none. */
static int g_crash_written = 0;

#ifdef AC_DEBUG_LOG
/* Echo to the real console even though stdout may be pointed at /dev/null.
 * pc_main.c redirects stdout when not verbose, so a plain printf would vanish. */
static void pc_log_console(const char* line) {
    fprintf(stderr, "%s\n", line);
    fflush(stderr);
}
#endif

/* Creates every missing component of an absolute-ish path. */
static void pc_log_mkdirs(const char* path) {
    char tmp[AC_LOG_MAX_PATH];
    size_t len = strlen(path);
    size_t i;

    if (len == 0 || len >= sizeof(tmp)) {
        return;
    }
    memcpy(tmp, path, len + 1);

    for (i = 1; i <= len; i++) {
        if (tmp[i] == '/' || tmp[i] == '\0') {
            char saved = tmp[i];
            tmp[i] = '\0';
            if (tmp[0] != '\0') {
                AC_MKDIR(tmp);
            }
            tmp[i] = saved;
        }
    }
}

static const char* pc_log_dir(void) {
    if (!g_log_dir_ready) {
#ifdef __ANDROID__
        const char* ext = SDL_AndroidGetExternalStoragePath();
        if (ext != NULL) {
            snprintf(g_log_dir, sizeof(g_log_dir), "%s/logs", ext);
        } else {
            snprintf(g_log_dir, sizeof(g_log_dir), "logs");
        }
#else
        snprintf(g_log_dir, sizeof(g_log_dir), "logs");
#endif
        g_log_dir_ready = 1;
        pc_log_mkdirs(g_log_dir);
    }
    return g_log_dir;
}

static void pc_log_stamp(char* out, size_t out_sz) {
    time_t now = time(NULL);
    struct tm tm_now;
#ifdef _WIN32
    tm_now = *localtime(&now);
#else
    localtime_r(&now, &tm_now);
#endif
    strftime(out, out_sz, "%Y-%m-%d-%H-%M-%S", &tm_now);
}

const char* pc_log_session_path(void) {
#ifdef AC_DEBUG_LOG
    return g_session_ready ? g_session_path : NULL;
#else
    return NULL;
#endif
}

/* ------------------------------------------------------------------------ */
/* Verbose session logging (AC_DEBUG_LOG only)                             */
/* ------------------------------------------------------------------------ */

#ifdef AC_DEBUG_LOG
void pc_log_init(void) {
    char stamp[32];

    if (g_session_ready) {
        return;
    }
    g_session_ready = 1;

    pc_log_stamp(stamp, sizeof(stamp));
    /* Bound the directory part so the compiler can prove the result fits; a
     * path that long is not a path anyone can open anyway. */
    snprintf(g_session_path, sizeof(g_session_path), "%.*s/session-%s.log",
             (int)(AC_LOG_MAX_PATH / 2), pc_log_dir(), stamp);
    g_session = fopen(g_session_path, "w");
    if (g_session == NULL) {
        /* Losing the log must never stop the game from starting. */
        pc_log_console("[PC] could not open session log; continuing without it");
        g_session_ready = 0;
        return;
    }
    fprintf(g_session, "[%s] Animal Crossing PC port - session log\n", stamp);
    fflush(g_session);
    pc_log_console(g_session_path);
}

static void pc_log_ring_push(const char* line) {
    /* Keep the tail only; a line longer than the slot is truncated rather
     * than wrapped, so the record stays greppable. */
    snprintf(g_ring[g_ring_next], AC_LOG_LINE_LEN, "%s", line);
    g_ring_next = (g_ring_next + 1) % AC_LOG_RING_LINES;
    if (g_ring_count < AC_LOG_RING_LINES) {
        g_ring_count++;
    }
}

/* Shared tail of pc_logf()/pc_vlogf(). */
static void pc_log_emit(const char* line) {
    char trimmed[AC_LOG_LINE_LEN];
    size_t len;

    snprintf(trimmed, sizeof(trimmed), "%s", line);
    len = strlen(trimmed);
    while (len > 0 && (trimmed[len - 1] == '\n' || trimmed[len - 1] == '\r')) {
        trimmed[--len] = '\0';
    }

    pc_log_console(trimmed);
    if (g_session != NULL) {
        fprintf(g_session, "%s\n", trimmed);
    }
    pc_log_ring_push(trimmed);
}

void pc_logf(const char* fmt, ...) {
    char line[AC_LOG_LINE_LEN];
    va_list args;

    va_start(args, fmt);
    vsnprintf(line, sizeof(line), fmt, args);
    va_end(args);

    pc_log_emit(line);
}

void pc_vlogf(const char* fmt, va_list args) {
    char line[AC_LOG_LINE_LEN];

    vsnprintf(line, sizeof(line), fmt, args);
    pc_log_emit(line);
}
#else
void pc_log_init(void) {
    /* No session log in release builds; crash records still work. */
}
#endif /* AC_DEBUG_LOG */

/* ------------------------------------------------------------------------ */
/* Crash records (always compiled in)                                       */
/* ------------------------------------------------------------------------ */

static const char* doing_point_name(u8 point) {
    switch (point) {
        case GRAPH_DOING_ZERO: return "ZERO";
        case GRAPH_DOING_CT: return "CT";
        case GRAPH_DOING_GAME_CT: return "GAME_CT";
        case GRAPH_DOING_GAME_CT_FINISHED: return "GAME_CT_FINISHED";
        case GRAPH_DOING_GAME_MAIN: return "GAME_MAIN";
        case GRAPH_DOING_GAME_TIME: return "GAME_TIME";
        case GRAPH_DOING_GAME_TIME_FINISHED: return "GAME_TIME_FINISHED";
        case GRAPH_DOING_GAME_EXEC: return "GAME_EXEC";
        case GRAPH_DOING_GAME_EXEC_FINISHED: return "GAME_EXEC_FINISHED";
        case GRAPH_DOING_GAME_BGM: return "GAME_BGM";
        case GRAPH_DOING_GAME_BGM_FINISHED: return "GAME_BGM_FINISHED";
        case GRAPH_DOING_GAME_MAIN_FINISHED: return "GAME_MAIN_FINISHED";
        case GRAPH_DOING_TASK_SET: return "TASK_SET";
        case GRAPH_DOING_WAIT_TASK: return "WAIT_TASK";
        case GRAPH_DOING_WAIT_TASK_FINISHED: return "WAIT_TASK_FINISHED";
        case GRAPH_DOING_TASK_SET_FINISHED: return "TASK_SET_FINISHED";
        case GRAPH_DOING_AUDIO: return "AUDIO";
        case GRAPH_DOING_AUDIO_FINISHED: return "AUDIO_FINISHED";
        case GRAPH_DOING_GAME_18: return "GAME_18";
        case GRAPH_DOING_GAME_DT: return "GAME_DT";
        case GRAPH_DOING_GAME_DT_FINISHED: return "GAME_DT_FINISHED";
        case GRAPH_DOING_DT: return "DT";
        case GRAPH_DOING_END: return "END";
        default: return "?";
    }
}

/* Header, game state, ring buffer, then the arena - in that order. Everything
 * above the arena query is a plain struct read, so a corrupt heap still leaves
 * a usable record. */
static FILE* pc_log_open_record(char* path, size_t path_sz, const char* kind,
                                const char* file, int line, const char* detail) {
    char stamp[32];
    FILE* out;

    /* Before the fopen below, which may well fail. */
    pc_save_note_crashed_session();

#ifdef AC_DEBUG_LOG
    /* Still in the FILE buffer, and we are about to die without flushing. */
    if (g_session != NULL) {
        fflush(g_session);
    }
#endif

    pc_log_stamp(stamp, sizeof(stamp));
    snprintf(path, path_sz, "%.*s/crash-%s.log",
             (int)(AC_LOG_MAX_PATH / 2), pc_log_dir(), stamp);
    out = fopen(path, "w");
    if (out == NULL) {
        return NULL;
    }

    fprintf(out, "[%s] ================ CRASH (%s) ================\n", stamp, kind);
    if (file != NULL) {
        fprintf(out, "  at %s:%d\n", file, line);
    }
    if (detail != NULL && *detail) {
        fprintf(out, "  %s\n", detail);
    }

    fprintf(out, "  game        : %p\n", (void*)game_class_p);
    if (game_class_p != NULL) {
        fprintf(out, "  frame       : %u\n", game_class_p->frame_counter);
    }
    fprintf(out, "  doing_point : %s (%u)\n",
            doing_point_name(graph_class.doing_point), graph_class.doing_point);
    fprintf(out, "  scene       : %d\n", Save_Get(scene_no));

#ifdef AC_DEBUG_LOG
    if (g_session_path[0]) {
        fprintf(out, "  session log : %s\n", g_session_path);
    }
#endif

    fflush(out);
    (void)stamp;
    return out;
}

static void pc_log_write_tail(FILE* out) {
#ifdef AC_DEBUG_LOG
    if (g_ring_count > 0) {
        int i;
        fprintf(out, "  --- last %d log lines ---\n", g_ring_count);
        /* Oldest first. */
        for (i = 0; i < g_ring_count; i++) {
            int idx = (g_ring_next - g_ring_count + i + AC_LOG_RING_LINES) % AC_LOG_RING_LINES;
            fprintf(out, "  | %s\n", g_ring[idx]);
        }
    }
#else
    fprintf(out, "  (built without AC_DEBUG_LOG: no session log or ring buffer.\n"
                 "   Rebuild with -DAC_DEBUG_LOG=ON to capture what led up to this.)\n");
#endif

    fflush(out);

    /* Last: the arena is what may be corrupted, and if this faults everything
     * above is already on disk. */
    {
        /* Not "capacity / free / used": biggest single free block, total free,
         * total used. So don't read the first as a total. */
        size_t biggest_free = 0;
        size_t total_free = 0;
        size_t total_used = 0;
        zelda_GetFreeArena(&biggest_free, &total_free, &total_used);
        fprintf(out, "  arena       : biggest free block=%zu, total free=%zu, total used=%zu\n",
                biggest_free, total_free, total_used);
    }
}

static void pc_log_console_report(const char* head, const char* path) {
    fprintf(stderr, "%s\n", head);
    fprintf(stderr, "Crash report: %s\n", path);
    fflush(stderr);
}

const char* pc_log_crash(const char* file, int line, const char* msg, va_list args) {
    static char path[AC_LOG_MAX_PATH];
    char detail[512];
    char head[600];
    FILE* out;

    if (g_crash_written) {
        return path[0] ? path : NULL;
    }
    g_crash_written = 1;

    /* Format the panic message first: vfprintf needs a live va_list and we can
     * only consume it once. */
    if (msg != NULL) {
        vsnprintf(detail, sizeof(detail), msg, args);
    } else {
        detail[0] = '\0';
    }
    snprintf(head, sizeof(head), "OSPanic at %s:%d: %s", file ? file : "?", line, detail);

    out = pc_log_open_record(path, sizeof(path), "OSPanic", file, line, detail);
    if (out == NULL) {
        /* Still give the message somewhere to go. */
        fprintf(stderr, "%s\n(could not open %s for writing)\n", head, path);
        fflush(stderr);
        path[0] = '\0';
        return NULL;
    }

    pc_log_write_tail(out);
    fclose(out);

    /* Mirror to the console so it's visible even if the dialog can't show. */
    pc_log_console_report(head, path);

    return path;
}

const char* pc_log_fault(const char* kind, uintptr_t addr, uintptr_t data_addr) {
    static char path[AC_LOG_MAX_PATH];
    char detail[256];
    char head[320];
    FILE* out;

    if (g_crash_written) {
        return path[0] ? path : NULL;
    }
    g_crash_written = 1;

    snprintf(detail, sizeof(detail), "%s at fault address %p (data %p)", kind, (void*)addr, (void*)data_addr);
    snprintf(head, sizeof(head), "%s", detail);

    out = pc_log_open_record(path, sizeof(path), "SIGNAL", NULL, 0, detail);
    if (out == NULL) {
        fprintf(stderr, "%s\n(could not open %s for writing)\n", head, path);
        fflush(stderr);
        path[0] = '\0';
        return NULL;
    }

    pc_log_write_tail(out);
    fclose(out);

    pc_log_console_report(head, path);

    return path;
}

void pc_log_show_crash_dialog(const char* log_path, const char* file, int line) {
    char msg[1024];
    const char* shown = (log_path != NULL && log_path[0]) ? log_path : "(no log file could be written)";

    if (file != NULL) {
        snprintf(msg, sizeof(msg),
                 "The game hit an internal error and had to stop.\n\n"
                 "  Error : %s line %d\n"
                 "  Report: %s\n\n"
                 "This is a bug in the port, not something you did wrong. "
                 "The report above is what a developer needs to fix it.",
                 file, line, shown);
    } else {
        snprintf(msg, sizeof(msg),
                 "The game hit an internal error and had to stop.\n\n"
                 "  Report: %s\n\n"
                 "This is a bug in the port, not something you did wrong. "
                 "The report above is what a developer needs to fix it.",
                 shown);
    }

    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Animal Crossing - Internal Error", msg, NULL);
}