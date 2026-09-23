/* pc_touch.c - virtual on-screen touch controls (Android overlay)
 *
 * Draws the v1 overlay (virtual stick + A/B/X/Start) with the GLES renderer
 * and merges touch state into the pad state the game reads (PADRead in
 * pc_pad.c). Zones are data TouchZone[]), not hardcoded geometry, so a
 * future button editor can serialize/deserialize the table.
 *
 * Built only for Android (wired from jni/CMakeLists.txt); GLES3 only.
 */
#include "android_touch.h"
#include <dolphin/pad.h>
#include <math.h>

#ifdef __ANDROID__
#include <jni.h>
#include <SDL_system.h>
#endif

#define ANDROID_TOUCH_STICK_MAG   80   /* matches STICK_MAGNITUDE in pc_pad.c */
#define ANDROID_TOUCH_MAX_FINGERS 8
#define ANDROID_TOUCH_SEGMENTS    32

typedef enum {
    TZ_STICK = 0,
    TZ_BUTTON
} TouchZoneKind;

typedef struct {
    const char* id;
    TouchZoneKind kind;
    float cx, cy;       /* center, normalized 0..1 of the window */
    float r;            /* radius in units of min(window w,h) */
    u16   pad_bit;      /* PAD_* bit (buttons/triggers), 0 for the stick */
    int   visible;      /* 1 = drawn + active (v1); 0 = prepared, not drawn */
    u8    color[4];     /* RGBA */
} TouchZone;

/* Default landscape layout (phone). D-pad/Y/L/R/Z are prepared (visible=0)
 * so they can be enabled without moving the v1 set. */
static TouchZone s_zones[] = {
    { "stick",   TZ_STICK,  0.16f, 0.55f, 0.16f,  0,                 1, {255,255,255, 90} },
    { "A",       TZ_BUTTON, 0.86f, 0.62f, 0.09f,  PAD_BUTTON_A,       1, { 90,200,120,200} },
    { "B",       TZ_BUTTON, 0.93f, 0.45f, 0.09f,  PAD_BUTTON_B,       1, {220, 90, 90,200} },
    { "X",       TZ_BUTTON, 0.79f, 0.45f, 0.09f,  PAD_BUTTON_X,       1, { 90,130,220,200} },
    { "Start",   TZ_BUTTON, 0.50f, 0.10f, 0.055f, PAD_BUTTON_START,   1, {255,255,255,170} },
    /* prepared for later */
    { "Y",       TZ_BUTTON, 0.86f, 0.30f, 0.09f,  PAD_BUTTON_Y,       0, {230,190, 70,200} },
    { "L",       TZ_BUTTON, 0.34f, 0.10f, 0.055f, PAD_TRIGGER_L,      0, {220,220,220,150} },
    { "R",       TZ_BUTTON, 0.66f, 0.10f, 0.055f, PAD_TRIGGER_R,      0, {220,220,220,150} },
    { "Z",       TZ_BUTTON, 0.66f, 0.30f, 0.055f, PAD_TRIGGER_Z,      0, {220,220,220,150} },
    { "D-Up",    TZ_BUTTON, 0.16f, 0.85f, 0.040f, PAD_BUTTON_UP,      0, {255,255,255,120} },
    { "D-Down",  TZ_BUTTON, 0.16f, 0.25f, 0.040f, PAD_BUTTON_DOWN,    0, {255,255,255,120} },
    { "D-Left",  TZ_BUTTON, 0.10f, 0.55f, 0.040f, PAD_BUTTON_LEFT,    0, {255,255,255,120} },
    { "D-Right", TZ_BUTTON, 0.22f, 0.55f, 0.040f, PAD_BUTTON_RIGHT,   0, {255,255,255,120} },
};
#define ANDROID_TOUCH_ZONE_COUNT (sizeof(s_zones) / sizeof(s_zones[0]))

typedef struct {
    SDL_FingerID finger;
    int   active;         /* finger currently down */
    int   zone;           /* -1 = not on any zone */
    float base_x, base_y; /* normalized, captured on touch-down */
    float cur_x, cur_y;   /* normalized, current */
} TouchFinger;

static TouchFinger s_fingers[ANDROID_TOUCH_MAX_FINGERS];

/* --- GL resources (same context as the game) --- */
static GLuint s_prog = 0, s_vao = 0, s_vbo = 0;
static GLint  s_u_res = -1, s_u_color = -1;

static const char* s_vs_src =
    "#version 300 es\n"
    "layout(location=0) in vec2 a_pos;\n"
    "uniform vec2 u_res;\n"
    "void main() {\n"
    "    vec2 ndc = (a_pos / u_res) * 2.0 - 1.0;\n"
    "    ndc.y = -ndc.y;\n"
    "    gl_Position = vec4(ndc, 0.0, 1.0);\n"
    "}\n";

static const char* s_fs_src =
    "#version 300 es\n"
    "precision mediump float;\n"
    "uniform vec4 u_color;\n"
    "out vec4 frag_color;\n"
    "void main() { frag_color = u_color; }\n";

static GLuint compile_shader(GLenum type, const char* src) {
    GLuint sh = glCreateShader(type);
    glShaderSource(sh, 1, &src, NULL);
    glCompileShader(sh);
    GLint ok = 0;
    glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetShaderInfoLog(sh, sizeof(log), NULL, log);
        fprintf(stderr, "[Touch] shader compile error: %s\n", log);
        glDeleteShader(sh);
        return 0;
    }
    return sh;
}

static int touch_gl_init(void) {
    if (s_prog) return 1;

    GLuint vs = compile_shader(GL_VERTEX_SHADER, s_vs_src);
    GLuint fs = compile_shader(GL_FRAGMENT_SHADER, s_fs_src);
    if (!vs || !fs) return 0;

    s_prog = glCreateProgram();
    glAttachShader(s_prog, vs);
    glAttachShader(s_prog, fs);
    glLinkProgram(s_prog);
    glDeleteShader(vs);
    glDeleteShader(fs);

    GLint ok = 0;
    glGetProgramiv(s_prog, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetProgramInfoLog(s_prog, sizeof(log), NULL, log);
        fprintf(stderr, "[Touch] program link error: %s\n", log);
        glDeleteProgram(s_prog);
        s_prog = 0;
        return 0;
    }

    s_u_res   = glGetUniformLocation(s_prog, "u_res");
    s_u_color = glGetUniformLocation(s_prog, "u_color");

    glGenVertexArrays(1, &s_vao);
    glGenBuffers(1, &s_vbo);
    glBindVertexArray(s_vao);
    glBindBuffer(GL_ARRAY_BUFFER, s_vbo);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, NULL);
    glEnableVertexAttribArray(0);
    glBindVertexArray(0);
    return 1;
}

/* window size in pixels (fallback to the GC frame buffer) */
static void win_size(int* w, int* h) {
    *w = (g_pc_window_w > 0) ? g_pc_window_w : PC_SCREEN_WIDTH;
    *h = (g_pc_window_h > 0) ? g_pc_window_h : PC_SCREEN_HEIGHT;
}

static int hit_test(float fx, float fy) {
    int w, h;
    win_size(&w, &h);
    float min_dim = (w < h) ? (float)w : (float)h;
    for (int i = 0; i < (int)ANDROID_TOUCH_ZONE_COUNT; i++) {
        TouchZone* z = &s_zones[i];
        if (!z->visible) continue;
        float dx = (fx - z->cx) * (float)w;
        float dy = (fy - z->cy) * (float)h;
        float rad = z->r * min_dim * 1.25f; /* 25% forgiveness */
        if (dx * dx + dy * dy <= rad * rad) return i;
    }
    return -1;
}

static int zone_held(int idx) {
    for (int i = 0; i < ANDROID_TOUCH_MAX_FINGERS; i++)
        if (s_fingers[i].active && s_fingers[i].zone == idx) return 1;
    return 0;
}

void android_touch_init(void) {
    memset(s_fingers, 0, sizeof(s_fingers));
    (void)touch_gl_init(); /* overlay just won't draw if GL setup fails */
}

/* Launch the Android system file picker (SAF). Called after the "No ROM found"
 * message box is answered with "Select ROM...". The picked file is copied to
 * <external>/rom/ by SDLActivity, which then restarts the app (or exits if the
 * user backed out). This call returns immediately. */
void android_rom_pick(void) {
    JNIEnv* env = (JNIEnv*)SDL_AndroidGetJNIEnv();
    if (!env) return;
    void* act = SDL_AndroidGetActivity();
    if (!act) return;

    jclass cls = (*env)->GetObjectClass(env, (jobject)act);
    jmethodID mid = (*env)->GetStaticMethodID(env, cls, "pickRom", "()V");
    if (mid) {
        (*env)->CallStaticVoidMethod(env, cls, mid);
    } else if ((*env)->ExceptionCheck(env)) {
        (*env)->ExceptionClear(env);
    }
    (*env)->DeleteLocalRef(env, cls);
}

void android_touch_shutdown(void) {
    if (s_vao) { glDeleteVertexArrays(1, &s_vao); s_vao = 0; }
    if (s_vbo) { glDeleteBuffers(1, &s_vbo); s_vbo = 0; }
    if (s_prog) { glDeleteProgram(s_prog); s_prog = 0; }
    memset(s_fingers, 0, sizeof(s_fingers));
}

void android_touch_handle_event(const SDL_Event* ev) {
    switch (ev->type) {
    case SDL_FINGERDOWN: {
        TouchFinger* f = NULL;
        for (int i = 0; i < ANDROID_TOUCH_MAX_FINGERS; i++) {
            if (!s_fingers[i].active) { f = &s_fingers[i]; break; }
        }
        if (!f) break;
        f->finger = ev->tfinger.fingerId;
        f->active = 1;
        f->zone = hit_test(ev->tfinger.x, ev->tfinger.y);
        /* a zone can only be held by one finger at a time */
        if (f->zone >= 0) {
            for (int i = 0; i < ANDROID_TOUCH_MAX_FINGERS; i++) {
                if (&s_fingers[i] != f && s_fingers[i].active &&
                    s_fingers[i].zone == f->zone) {
                    f->zone = -1;
                    break;
                }
            }
        }
        f->base_x = f->cur_x = ev->tfinger.x;
        f->base_y = f->cur_y = ev->tfinger.y;
        break;
    }
    case SDL_FINGERMOTION:
        for (int i = 0; i < ANDROID_TOUCH_MAX_FINGERS; i++) {
            if (s_fingers[i].active && s_fingers[i].finger == ev->tfinger.fingerId) {
                s_fingers[i].cur_x = ev->tfinger.x;
                s_fingers[i].cur_y = ev->tfinger.y;
                break;
            }
        }
        break;
    case SDL_FINGERUP:
        for (int i = 0; i < ANDROID_TOUCH_MAX_FINGERS; i++) {
            if (s_fingers[i].active && s_fingers[i].finger == ev->tfinger.fingerId) {
                s_fingers[i].active = 0;
                s_fingers[i].zone = -1;
                break;
            }
        }
        break;
    }
}

void android_touch_apply(u16* buttons, s8* stick_x, s8* stick_y) {
    int w, h;
    win_size(&w, &h);
    float min_dim = (w < h) ? (float)w : (float)h;
    int   stick_active = 0;
    float sx = 0, sy = 0;

    for (int i = 0; i < ANDROID_TOUCH_MAX_FINGERS; i++) {
        TouchFinger* f = &s_fingers[i];
        if (!f->active || f->zone < 0 || f->zone >= (int)ANDROID_TOUCH_ZONE_COUNT) continue;
        TouchZone* z = &s_zones[f->zone];
        if (!z->visible) continue;

        if (z->kind == TZ_BUTTON) {
            if (z->pad_bit) *buttons |= z->pad_bit;
        } else if (z->kind == TZ_STICK) {
            /* displacement from touch-down point, in pixels */
            float dx = (f->cur_x - f->base_x) * (float)w;
            float dy = (f->cur_y - f->base_y) * (float)h;
            float rad = z->r * min_dim;
            float dist = sqrtf(dx * dx + dy * dy);
            float dead = rad * 0.15f;
            if (dist > dead) {
                float mag = (dist - dead) / (rad - dead); /* 0..1 */
                if (mag > 1.0f) mag = 1.0f;
                float scale = ANDROID_TOUCH_STICK_MAG * mag / dist;
                sx =  dx * scale;
                sy = -dy * scale; /* SDL touch Y grows down; stickY>0 = up */
                stick_active = 1;
            }
        }
    }

    if (stick_active) {
        *stick_x = (s8)sx;
        *stick_y = (s8)sy;
    }
}

static void draw_circle(float cx, float cy, float rad, const float rgba[4]) {
    int   n = ANDROID_TOUCH_SEGMENTS;
    int   count = n + 2;
    float verts[(ANDROID_TOUCH_SEGMENTS + 2) * 2];
    verts[0] = cx;
    verts[1] = cy;
    for (int i = 0; i <= n; i++) {
        float a = (float)i / (float)n * 2.0f * PC_PIf;
        verts[(i + 1) * 2 + 0] = cx + cosf(a) * rad;
        verts[(i + 1) * 2 + 1] = cy + sinf(a) * rad;
    }
    glBindBuffer(GL_ARRAY_BUFFER, s_vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_DYNAMIC_DRAW);
    glUniform4fv(s_u_color, 1, rgba);
    glDrawArrays(GL_TRIANGLE_FAN, 0, count);
}

void android_touch_draw(void) {
    if (!s_prog) return;

    int w, h;
    win_size(&w, &h);
    float min_dim = (w < h) ? (float)w : (float)h;

    glDisable(GL_DEPTH_TEST);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_CULL_FACE);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glViewport(0, 0, w, h);
    glUseProgram(s_prog);
    glUniform2f(s_u_res, (float)w, (float)h);
    glBindVertexArray(s_vao);

    for (int i = 0; i < (int)ANDROID_TOUCH_ZONE_COUNT; i++) {
        TouchZone* z = &s_zones[i];
        if (!z->visible) continue;

        float cx = z->cx * (float)w;
        float cy = z->cy * (float)h;
        float rad = z->r * min_dim;
        float r = z->color[0] / 255.f;
        float g = z->color[1] / 255.f;
        float b = z->color[2] / 255.f;
        float a = z->color[3] / 255.f;
        int   pressed = zone_held(i);

        if (z->kind == TZ_STICK) {
            float base[4] = { r, g, b, a * 0.35f };
            float ring[4] = { r, g, b, a };
            float hole[4] = { 0.04f, 0.04f, 0.04f, a };
            float knob[4] = { 1.0f, 1.0f, 1.0f, 0.9f };

            /* knob follows the finger holding the stick */
            float kx = cx, ky = cy;
            for (int f = 0; f < ANDROID_TOUCH_MAX_FINGERS; f++) {
                TouchFinger* pf = &s_fingers[f];
                if (pf->active && pf->zone == i) {
                    float dx = (pf->cur_x - pf->base_x) * (float)w;
                    float dy = (pf->cur_y - pf->base_y) * (float)h;
                    float d = sqrtf(dx * dx + dy * dy);
                    float maxoff = rad * 0.55f;
                    if (d > maxoff && d > 0.0001f) { dx *= maxoff / d; dy *= maxoff / d; }
                    kx = cx + dx;
                    ky = cy + dy;
                    break;
                }
            }
            draw_circle(cx, cy, rad, base);
            draw_circle(cx, cy, rad, ring);
            draw_circle(cx, cy, rad - min_dim * 0.045f, hole);
            draw_circle(kx, ky, rad * 0.38f, knob);
        } else { /* button */
            float col[4] = { r, g, b, a + (pressed ? 0.35f : 0.0f) };
            if (col[3] > 1.0f) col[3] = 1.0f;
            draw_circle(cx, cy, rad * (pressed ? 0.95f : 1.0f), col);
        }
    }

    glBindVertexArray(0);
    glDisable(GL_BLEND);
}