/*
 * Osmo Remote — Nintendo Switch viewer for the DJI Osmo Pocket over Wi-Fi.
 *
 * Mac side (mac/stream_server.py) serves:
 *   GET /stream          multipart MJPEG
 *   GET /ptz?dx=&dy=     relative gimbal move (degrees)
 *   GET /ptz?action=center
 *   GET /config          resolution / fps / quality
 *
 * Controls:
 *   left stick / D-pad  pan + tilt
 *   X                   recenter gimbal
 *   +                   settings overlay
 *   A                   connect / start stream
 *   B                   close overlay / stop stream / exit
 */

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include <switch.h>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_JPEG
#define STBI_NO_THREAD_LOCALS
#define STBI_NO_HDR
#define STBI_NO_LINEAR
#include "stb_image.h"
#include "stb_easy_font.h"

/* Match the Mac's --scale so we can blit 1:1 (no resampling). */
#define FB_W 1280
#define FB_H 720
#define DEFAULT_HOST "192.168.100.6"
#define DEFAULT_PORT 8765
#define HOST_PATH "sdmc:/switch/osmo-remote-host.txt"
#define HOST_PATH_ALT "sdmc:/switch/osmo-view-host.txt"
#define MAX_JPEG (1024 * 1024)
#define RECV_CHUNK 16384

/* Target frame time in microseconds, driven by the fps setting. */
static int g_frame_us = 33333;

/* Fill a rect in the RGBA framebuffer. */
static void fill_rect(u8 *base, u32 stride_bytes, int x0, int y0, int x1, int y1, u32 color) {
    if (!base)
        return;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > FB_W - 1) x1 = FB_W - 1;
    if (y1 > FB_H - 1) y1 = FB_H - 1;
    for (int y = y0; y <= y1; y++) {
        u32 *row = (u32 *)(base + (size_t)y * stride_bytes);
        for (int x = x0; x <= x1; x++)
            row[x] = color;
    }
}

/* Draw text via stb_easy_font quads, each quad filled as a rect. */
static void draw_text(u8 *base, u32 stride_bytes, int x, int y, const char *text, u32 fg) {
    static char vbuf[16384];
    int quads = stb_easy_font_print(0, 0, (char *)text, NULL, vbuf, sizeof(vbuf));
    const float *v = (const float *)vbuf;
    for (int q = 0; q < quads; q++) {
        const float *p = v + q * 16;
        float minx = p[0], maxx = p[0], miny = p[1], maxy = p[1];
        for (int i = 1; i < 4; i++) {
            float px = p[i * 4];
            float py = p[i * 4 + 1];
            if (px < minx) minx = px;
            if (px > maxx) maxx = px;
            if (py < miny) miny = py;
            if (py > maxy) maxy = py;
        }
        fill_rect(base, stride_bytes, x + (int)minx, y + (int)miny,
                  x + (int)maxx, y + (int)maxy, fg);
    }
}

typedef struct {
    char host[64];
    int port;
} Config;

static Config g_cfg;
static char g_status[160] = "A: conectar  X: centralizar  B: sair";
static volatile int g_ptz_ready = 0;

/* gimbal speed multipliers, adjustable in the settings menu */
static float g_speed = 1.0f;
static const float SPEEDS[] = {0.5f, 0.8f, 1.0f, 1.5f, 2.2f, 3.0f};
#define SPEED_COUNT ((int)(sizeof(SPEEDS) / sizeof(SPEEDS[0])))

static void set_status(const char *s) {
    snprintf(g_status, sizeof(g_status), "%s", s);
}

static void config_defaults(void) {
    snprintf(g_cfg.host, sizeof(g_cfg.host), "%s", DEFAULT_HOST);
    g_cfg.port = DEFAULT_PORT;
}

static void config_load(void) {
    config_defaults();
    FILE *f = fopen(HOST_PATH, "r");
    if (!f)
        f = fopen(HOST_PATH_ALT, "r");
    if (!f)
        return;
    char line[128];
    if (fgets(line, sizeof(line), f)) {
        char *nl = strchr(line, '\n');
        if (nl)
            *nl = 0;
        char *colon = strrchr(line, ':');
        if (colon && strchr(line, '.')) {
            *colon = 0;
            if (line[0])
                snprintf(g_cfg.host, sizeof(g_cfg.host), "%s", line);
            int p = atoi(colon + 1);
            if (p > 0 && p < 65536)
                g_cfg.port = p;
        } else if (line[0]) {
            snprintf(g_cfg.host, sizeof(g_cfg.host), "%s", line);
        }
    }
    fclose(f);
}

static void config_save(void) {
    mkdir("sdmc:/switch", 0755);
    FILE *f = fopen(HOST_PATH, "w");
    if (!f)
        return;
    fprintf(f, "%s:%d\n", g_cfg.host, g_cfg.port);
    fclose(f);
}

static void bump_last_octet(int delta) {
    unsigned a, b, c, d;
    if (sscanf(g_cfg.host, "%u.%u.%u.%u", &a, &b, &c, &d) != 4)
        return;
    int nd = (int)d + delta;
    if (nd < 1)
        nd = 254;
    if (nd > 254)
        nd = 1;
    snprintf(g_cfg.host, sizeof(g_cfg.host), "%u.%u.%u.%u", a, b, c, (unsigned)nd);
    config_save();
}

/* ------------------------------------------------------------------ */
/* Gimbal control — own thread so PTZ latency never stalls the video.  */
/* ------------------------------------------------------------------ */

typedef struct {
    Mutex lock;
    float pending_dx;
    float pending_dy;
    int want_center;
    int stop;
    Thread thread;
    int running;
} PtzClient;

static PtzClient g_ptz;

static int http_get_once(const char *path, char *out, size_t out_len) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;

    struct timeval tv;
    tv.tv_sec = 2;
    tv.tv_usec = 0;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)g_cfg.port);
    if (inet_pton(AF_INET, g_cfg.host, &addr.sin_addr) != 1) {
        close(fd);
        return -1;
    }
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(fd);
        return -1;
    }

    char req[320];
    snprintf(req, sizeof(req),
             "GET %s HTTP/1.0\r\nHost: %s:%d\r\nUser-Agent: OsmoView-NX\r\n\r\n",
             path, g_cfg.host, g_cfg.port);
    if (send(fd, req, strlen(req), 0) < 0) {
        close(fd);
        return -1;
    }

    int total = 0;
    while (total < (int)out_len - 1) {
        ssize_t n = recv(fd, out + total, out_len - 1 - (size_t)total, 0);
        if (n <= 0)
            break;
        total += (int)n;
    }
    out[total > 0 ? total : 0] = 0;
    close(fd);
    return total;
}

static void ptz_thread_fn(void *arg) {
    (void)arg;
    char resp[512];

    /* probe availability once */
    if (http_get_once("/ptz/state", resp, sizeof(resp)) > 0)
        g_ptz_ready = 1;

    while (!g_ptz.stop) {
        float dx = 0, dy = 0;
        int center = 0;
        mutexLock(&g_ptz.lock);
        dx = g_ptz.pending_dx;
        dy = g_ptz.pending_dy;
        center = g_ptz.want_center;
        g_ptz.pending_dx = 0;
        g_ptz.pending_dy = 0;
        g_ptz.want_center = 0;
        mutexUnlock(&g_ptz.lock);

        if (center) {
            http_get_once("/ptz?action=center", resp, sizeof(resp));
            svcSleepThread(30 * 1000 * 1000ULL);
            continue;
        }

        if (dx != 0.0f || dy != 0.0f) {
            char path[96];
            snprintf(path, sizeof(path), "/ptz?dx=%.2f&dy=%.2f", dx, dy);
            if (http_get_once(path, resp, sizeof(resp)) <= 0)
                g_ptz_ready = 0;
            else
                g_ptz_ready = 1;
            /* pace moves so the gimbal has time to settle */
            svcSleepThread(45 * 1000 * 1000ULL);
        } else {
            svcSleepThread(10 * 1000 * 1000ULL);
        }
    }
}

static void ptz_queue_move(float dx, float dy) {
    mutexLock(&g_ptz.lock);
    g_ptz.pending_dx += dx;
    g_ptz.pending_dy += dy;
    /* keep the queue short so input stays responsive */
    if (g_ptz.pending_dx > 25.0f)
        g_ptz.pending_dx = 25.0f;
    if (g_ptz.pending_dx < -25.0f)
        g_ptz.pending_dx = -25.0f;
    if (g_ptz.pending_dy > 25.0f)
        g_ptz.pending_dy = 25.0f;
    if (g_ptz.pending_dy < -25.0f)
        g_ptz.pending_dy = -25.0f;
    mutexUnlock(&g_ptz.lock);
}

static void ptz_queue_center(void) {
    mutexLock(&g_ptz.lock);
    g_ptz.want_center = 1;
    g_ptz.pending_dx = 0;
    g_ptz.pending_dy = 0;
    mutexUnlock(&g_ptz.lock);
}

static void ptz_client_start(void) {
    if (g_ptz.running)
        return;
    mutexInit(&g_ptz.lock);
    g_ptz.pending_dx = g_ptz.pending_dy = 0;
    g_ptz.want_center = 0;
    g_ptz.stop = 0;
    if (R_SUCCEEDED(threadCreate(&g_ptz.thread, ptz_thread_fn, NULL, NULL, 0x4000, 0x2C, -2))) {
        threadStart(&g_ptz.thread);
        g_ptz.running = 1;
    }
}

static void ptz_client_stop(void) {
    if (!g_ptz.running)
        return;
    g_ptz.stop = 1;
    threadWaitForExit(&g_ptz.thread);
    threadClose(&g_ptz.thread);
    g_ptz.running = 0;
}

/* ------------------------------------------------------------------ */
/* Rendering                                                           */
/* ------------------------------------------------------------------ */

/* framebufferBegin() returns a stride in BYTES (libnx docs) — the old
 * version treated it as pixels, which is what produced the gray banding. */
static void blit_rgb(u8 *base, u32 stride_bytes, const unsigned char *rgb, int w, int h) {
    if (!base || !rgb || w < 1 || h < 1)
        return;

    const u32 row_bytes = (u32)FB_W * 4u;
    if (stride_bytes < row_bytes)
        return;

    /* 1:1 when the Mac already scaled to FB size */
    if (w == FB_W && h == FB_H) {
        for (int y = 0; y < FB_H; y++) {
            u32 *dst = (u32 *)(base + (size_t)y * stride_bytes);
            const unsigned char *src = rgb + (size_t)y * w * 3u;
            for (int x = 0; x < FB_W; x++) {
                dst[x] = 0xFF000000u | ((u32)src[0]) | ((u32)src[1] << 8) | ((u32)src[2] << 16);
                src += 3;
            }
        }
        return;
    }

    /* nearest-neighbour letterbox for other sizes */
    for (int y = 0; y < FB_H; y++) {
        u32 *dst = (u32 *)(base + (size_t)y * stride_bytes);
        int syi = y * h / FB_H;
        if (syi >= h)
            syi = h - 1;
        const unsigned char *srow = rgb + (size_t)syi * w * 3u;
        for (int x = 0; x < FB_W; x++) {
            int sxi = x * w / FB_W;
            if (sxi >= w)
                sxi = w - 1;
            const unsigned char *p = srow + (size_t)sxi * 3u;
            dst[x] = 0xFF000000u | ((u32)p[0]) | ((u32)p[1] << 8) | ((u32)p[2] << 16);
        }
    }
}

static void clear_fb(Framebuffer *fb, u32 color) {
    u32 stride = 0;
    u8 *base = (u8 *)framebufferBegin(fb, &stride);
    if (!base) {
        framebufferEnd(fb);
        return;
    }
    for (int y = 0; y < FB_H; y++) {
        u32 *row = (u32 *)(base + (size_t)y * stride);
        for (int x = 0; x < FB_W; x++)
            row[x] = color;
    }
    framebufferEnd(fb);
}

/* ------------------------------------------------------------------ */
/* Streaming                                                           */
/* ------------------------------------------------------------------ */

static int find_jpeg(const unsigned char *buf, int len, int *out_start, int *out_end) {
    int start = -1;
    for (int i = 0; i + 1 < len; i++) {
        if (buf[i] == 0xFF && buf[i + 1] == 0xD8) {
            start = i;
            break;
        }
    }
    if (start < 0)
        return 0;
    for (int i = start + 2; i + 1 < len; i++) {
        if (buf[i] == 0xFF && buf[i + 1] == 0xD9) {
            int jlen = (i + 2) - start;
            if (jlen < 128)
                return 0;
            *out_start = start;
            *out_end = i + 2;
            return 1;
        }
        if (i - start > MAX_JPEG)
            return 0;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Settings menu — drawn as an overlay on top of the live picture      */
/* ------------------------------------------------------------------ */

typedef struct {
    int scale_idx;
    int fps_idx;
    int quality_idx;
    int speed_idx;
} Menu;

static const int SCALE_W[] = {640, 960, 1280};
static const int SCALE_H[] = {360, 540, 720};
#define SCALE_COUNT 3
/* Pocket 4 UVC only advertises 24/25/30 fps; other values make ffmpeg
   refuse to open the device. */
static const int FPS_OPTS[] = {24, 25, 30};
#define FPS_COUNT 3
static const int QUALITY_OPTS[] = {3, 5, 8, 12, 18};
#define QUALITY_COUNT 5
#define MENU_ITEMS 4

static int index_of(const float *arr, int n, float value) {
    for (int i = 0; i < n; i++)
        if (arr[i] == value)
            return i;
    return 0;
}

static void menu_defaults(Menu *m) {
    m->scale_idx = 2; /* 1280x720 */
    m->fps_idx = 2;   /* 30 */
    m->quality_idx = 1; /* 5 */
    m->speed_idx = index_of(SPEEDS, SPEED_COUNT, 1.0f);
    g_frame_us = 1000000 / FPS_OPTS[m->fps_idx];
}

static void config_get(const char *path) {
    char resp[512];
    http_get_once(path, resp, sizeof(resp));
}

static void menu_apply_scale(Menu *m) {
    char path[64];
    snprintf(path, sizeof(path), "/config?scale=%d:%d", SCALE_W[m->scale_idx],
             SCALE_H[m->scale_idx]);
    config_get(path);
}

static void menu_apply_fps(Menu *m) {
    char path[64];
    snprintf(path, sizeof(path), "/config?fps=%d", FPS_OPTS[m->fps_idx]);
    config_get(path);
    g_frame_us = 1000000 / FPS_OPTS[m->fps_idx];
}

static void menu_apply_quality(Menu *m) {
    char path[64];
    snprintf(path, sizeof(path), "/config?quality=%d", QUALITY_OPTS[m->quality_idx]);
    config_get(path);
}

static void menu_value(Menu *m, int item, char *out, size_t out_len) {
    switch (item) {
    case 0:
        snprintf(out, out_len, "%dx%d", SCALE_W[m->scale_idx], SCALE_H[m->scale_idx]);
        break;
    case 1:
        snprintf(out, out_len, "%d fps", FPS_OPTS[m->fps_idx]);
        break;
    case 2:
        snprintf(out, out_len, "%d", QUALITY_OPTS[m->quality_idx]);
        break;
    default:
        snprintf(out, out_len, "%.1fx", (double)SPEEDS[m->speed_idx]);
        break;
    }
}

static void overlay_menu(u8 *base, u32 stride, Menu *m, int cursor, const char *note) {
    static const char *labels[MENU_ITEMS] = {"Resolucao", "FPS", "Qualidade", "Velocidade"};
    const int pw = 500, ph = 260;
    const int px = (FB_W - pw) / 2, py = (FB_H - ph) / 2;

    fill_rect(base, stride, px, py, px + pw - 1, py + ph - 1, 0xF00A0A0A);
    fill_rect(base, stride, px, py, px + pw - 1, py + 2, 0xFF60C8FF);
    fill_rect(base, stride, px, py + ph - 3, px + pw - 1, py + ph - 1, 0xFF60C8FF);

    draw_text(base, stride, px + 16, py + 12, "CONFIGURACOES", 0xFF9FE0FF);

    for (int i = 0; i < MENU_ITEMS; i++) {
        char value[32];
        menu_value(m, i, value, sizeof(value));
        int ty = py + 56 + i * 34;
        if (i == cursor)
            fill_rect(base, stride, px + 10, ty - 6, px + pw - 11, ty + 18, 0xFF1E3A5F);
        draw_text(base, stride, px + 18, ty, (char *)labels[i], 0xFFE8E8E8);
        draw_text(base, stride, px + 270, ty, value, 0xFFFFD479);
    }

    draw_text(base, stride, px + 16, py + 206, "< > muda   A ok   X padrao", 0xFF9AA5B1);
    draw_text(base, stride, px + 16, py + 228, "Y turbo   B fecha", 0xFF9AA5B1);
    if (note && note[0])
        draw_text(base, stride, px + 290, py + 206, (char *)note, 0xFF7DE08A);
}

/* Returns 1 when settings changed (caller may want to reset pacing). */
static void menu_handle(Menu *m, u64 down, u64 held, int *cursor, char *note) {
    if (down & HidNpadButton_Up)
        *cursor = (*cursor + MENU_ITEMS - 1) % MENU_ITEMS;
    if (down & HidNpadButton_Down)
        *cursor = (*cursor + 1) % MENU_ITEMS;

    int step = 0;
    if (down & (HidNpadButton_Right | HidNpadButton_A))
        step = 1;
    if (down & HidNpadButton_Left)
        step = -1;

    if (step != 0) {
        if (*cursor == 0) {
            m->scale_idx = (m->scale_idx + step + SCALE_COUNT) % SCALE_COUNT;
            menu_apply_scale(m);
            snprintf(note, 80, "resolucao ok");
        } else if (*cursor == 1) {
            m->fps_idx = (m->fps_idx + step + FPS_COUNT) % FPS_COUNT;
            menu_apply_fps(m);
            snprintf(note, 80, "fps ok");
        } else if (*cursor == 2) {
            m->quality_idx = (m->quality_idx + step + QUALITY_COUNT) % QUALITY_COUNT;
            menu_apply_quality(m);
            snprintf(note, 80, "qualidade ok");
        } else {
            m->speed_idx = (m->speed_idx + step + SPEED_COUNT) % SPEED_COUNT;
            g_speed = SPEEDS[m->speed_idx];
            snprintf(note, 80, "velocidade ok");
        }
    }

    if (held & HidNpadButton_Right)
        svcSleepThread(140 * 1000 * 1000ULL);

    if (down & HidNpadButton_X) {
        menu_defaults(m);
        g_speed = SPEEDS[m->speed_idx];
        menu_apply_scale(m);
        menu_apply_fps(m);
        menu_apply_quality(m);
        snprintf(note, 80, "padroes");
    }

    if (down & HidNpadButton_Y) {
        config_get("/config?preset=turbo");
        m->scale_idx = 0;
        m->fps_idx = 2;
        m->quality_idx = 3;
        g_frame_us = 1000000 / FPS_OPTS[m->fps_idx];
        snprintf(note, 80, "turbo");
    }
}

/* ------------------------------------------------------------------ */

static void handle_stick(PadState *pad, int fast) {
    HidAnalogStickState ls = padGetStickPos(pad, 0);
    HidAnalogStickState rs = padGetStickPos(pad, 1);

    const float dead = 0.22f;
    float lx = (float)ls.x / 32767.0f;
    float ly = (float)ls.y / 32767.0f;
    float rx = (float)rs.x / 32767.0f;
    float ry = (float)rs.y / 32767.0f;

    if (lx > -dead && lx < dead)
        lx = 0.0f;
    if (ly > -dead && ly < dead)
        ly = 0.0f;
    if (rx > -dead && rx < dead)
        rx = 0.0f;
    if (ry > -dead && ry < dead)
        ry = 0.0f;

    float gain = (fast ? 2.2f : 1.0f) * g_speed;
    float dx = 0.0f, dy = 0.0f;
    if (lx != 0.0f || ly != 0.0f) {
        dx += lx * gain;
        dy += ly * gain;
    }
    if (rx != 0.0f || ry != 0.0f) {
        dx += rx * gain;
        dy += ry * gain;
    }

    u64 held = padGetButtons(pad);
    float step = 2.0f * g_speed;
    if (held & HidNpadButton_Left)
        dx -= step;
    if (held & HidNpadButton_Right)
        dx += step;
    if (held & HidNpadButton_Up)
        dy += step;
    if (held & HidNpadButton_Down)
        dy -= step;

    if (dx != 0.0f || dy != 0.0f)
        ptz_queue_move(dx, dy);
}

static void stream_loop(PadState *pad, Menu *menu) {
    char msg[128];
    snprintf(msg, sizeof(msg), "conectando %s:%d …", g_cfg.host, g_cfg.port);
    set_status(msg);

    NWindow *win = nwindowGetDefault();
    Framebuffer fb;
    if (R_FAILED(framebufferCreate(&fb, win, FB_W, FB_H, PIXEL_FORMAT_RGBA_8888, 2))) {
        set_status("framebufferCreate falhou");
        return;
    }
    framebufferMakeLinear(&fb);
    clear_fb(&fb, 0xFF101010u);

    int fd = -1;
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)g_cfg.port);
    if (inet_pton(AF_INET, g_cfg.host, &addr.sin_addr) != 1) {
        set_status("host invalido");
        framebufferClose(&fb);
        return;
    }
    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0 || connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        if (fd >= 0)
            close(fd);
        set_status("falhou conectar — Mac streamando? mesma Wi‑Fi?");
        framebufferClose(&fb);
        return;
    }
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    struct timeval tv;
    tv.tv_sec = 3;
    tv.tv_usec = 0;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    const char *req =
        "GET /stream HTTP/1.0\r\n"
        "Host: osmo\r\n"
        "User-Agent: OsmoView-NX\r\n"
        "\r\n";
    if (send(fd, req, strlen(req), 0) < 0) {
        close(fd);
        set_status("falhou enviar GET /stream");
        framebufferClose(&fb);
        return;
    }

    tv.tv_sec = 5;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    unsigned char *buf = (unsigned char *)malloc(MAX_JPEG + RECV_CHUNK);
    if (!buf) {
        close(fd);
        framebufferClose(&fb);
        set_status("sem memoria");
        return;
    }

    ptz_client_start();

    int buflen = 0;
    int frames = 0;
    int skipped = 0;
    int fast = 0;
    int menu_open = 0;
    int menu_cursor = 0;
    char menu_note[80] = "";
    u64 next_frame = svcGetSystemTick();

    set_status("ao vivo — + abre menu, stick move, X centraliza, B sai");

    while (appletMainLoop()) {
        padUpdate(pad);
        u64 down = padGetButtonsDown(pad);
        u64 held = padGetButtons(pad);

        if (down & HidNpadButton_B) {
            if (menu_open) {
                menu_open = 0;
                menu_note[0] = 0;
            } else {
                break;
            }
        }
        if (down & HidNpadButton_Plus) {
            menu_open = !menu_open;
            menu_cursor = 0;
            menu_note[0] = 0;
        }

        if (menu_open) {
            menu_handle(menu, down, held, &menu_cursor, menu_note);
        } else {
            if (down & HidNpadButton_X) {
                ptz_queue_center();
                set_status("centralizando…");
            }
            fast = (held & HidNpadButton_ZL) || (held & HidNpadButton_ZR);
            handle_stick(pad, fast);
        }

        if (buflen > MAX_JPEG - RECV_CHUNK) {
            int keep = MAX_JPEG / 2;
            memmove(buf, buf + (buflen - keep), (size_t)keep);
            buflen = keep;
        }

        ssize_t n = recv(fd, buf + buflen, RECV_CHUNK, 0);
        if (n < 0)
            continue;
        if (n == 0) {
            set_status("stream caiu — A reconecta");
            break;
        }
        buflen += (int)n;

        /* Latency killer: drain everything already queued in the socket and
         * render only the last frame. Without this the Switch works through a
         * backlog and the picture trails reality by seconds. */
        int drained = 0;
        while (drained < 8 * 1024 * 1024) {
            ssize_t more = recv(fd, buf + buflen, RECV_CHUNK, MSG_DONTWAIT);
            if (more <= 0)
                break;
            buflen += (int)more;
            drained += (int)more;
            if (buflen > MAX_JPEG - RECV_CHUNK) {
                int keep = MAX_JPEG / 2;
                memmove(buf, buf + (buflen - keep), (size_t)keep);
                buflen = keep;
            }
        }

        /* Render only the newest complete JPEG in the buffer. */
        int js = -1, je = -1;
        int scan = 0;
        while (scan < buflen) {
            int s2, e2;
            if (!find_jpeg(buf + scan, buflen - scan, &s2, &e2))
                break;
            js = scan + s2;
            je = scan + e2;
            scan = je;
        }
        if (js < 0) {
            if (buflen > MAX_JPEG - RECV_CHUNK) {
                int keep = MAX_JPEG / 4;
                memmove(buf, buf + (buflen - keep), (size_t)keep);
                buflen = keep;
            }
            continue;
        }

        int jlen = je - js;
        int w = 0, h = 0, comp = 0;
        unsigned char *rgb = stbi_load_from_memory(buf + js, jlen, &w, &h, &comp, 3);
        memmove(buf, buf + je, (size_t)(buflen - je));
        buflen -= je;

        if (!rgb) {
            skipped++;
            continue;
        }
        if (w < 16 || h < 16 || w > 4096 || h > 4096) {
            stbi_image_free(rgb);
            skipped++;
            continue;
        }

        u32 stride = 0;
        u8 *base = (u8 *)framebufferBegin(&fb, &stride);
        if (base) {
            blit_rgb(base, stride, rgb, w, h);
            if (menu_open)
                overlay_menu(base, stride, menu, menu_cursor, menu_note);
            framebufferEnd(&fb);
            frames++;
        }
        stbi_image_free(rgb);

        if ((frames & 15) == 0) {
            snprintf(g_status, sizeof(g_status), "ao vivo %dx%d f=%d skip=%d",
                     w, h, frames, skipped);
        }

        /* Pace to the configured fps: this is what actually keeps the picture
         * close to real time. svcGetSystemTick() counts at 19.2 MHz, so one
         * frame of g_frame_us microseconds is g_frame_us * 19.2 ticks. */
        next_frame += (u64)g_frame_us * 192ull / 10ull;
        u64 now = svcGetSystemTick();
        if (next_frame > now) {
            u64 wait_ticks = next_frame - now;
            u64 max_wait = 19200ull * 200ull; /* 200 ms safety cap */
            if (wait_ticks > max_wait)
                wait_ticks = max_wait;
            svcSleepThread(wait_ticks * 1000000000ull / 19200000ull);
        } else {
            next_frame = svcGetSystemTick();
        }
    }

    ptz_client_stop();
    free(buf);
    close(fd);
    framebufferClose(&fb);
    set_status("desconectado — A: conectar");
}

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;

    appletLockExit();

    padConfigureInput(2, HidNpadStyleSet_NpadStandard);
    PadState pad;
    padInitializeDefault(&pad);

    socketInitializeDefault();
    config_load();
    consoleInit(NULL);

    Menu menu;
    menu_defaults(&menu);
    g_speed = SPEEDS[menu.speed_idx];

    while (appletMainLoop()) {
        padUpdate(&pad);
        u64 k = padGetButtonsDown(&pad);

        consoleClear();
        printf("\x1b[1;1H");
        printf("==========================================\n");
        printf("  Osmo Remote  (Pocket no Switch)\n");
        printf("==========================================\n\n");
        printf("  Host: %s:%d\n\n", g_cfg.host, g_cfg.port);
        printf("  %s\n\n", g_status);
        printf("  A  conectar / ver\n");
        printf("  B  sair\n");
        printf("  +  configuracoes (resolucao/fps/velocidade)\n");
        printf("  -  ultimo octeto do IP\n");
        printf("  X  reload host | Y salvar\n");
        consoleUpdate(NULL);

        if (k & HidNpadButton_Plus) {
            set_status("dentro do stream, + abre o menu");
        }
        if (k & HidNpadButton_Minus)
            bump_last_octet(1);
        if (k & HidNpadButton_X) {
            config_load();
            set_status("host.txt recarregado");
        }
        if (k & HidNpadButton_Y) {
            config_save();
            set_status("host.txt salvo");
        }
        if (k & HidNpadButton_A) {
            consoleExit(NULL);
            svcSleepThread(50 * 1000 * 1000ULL);
            stream_loop(&pad, &menu);
            svcSleepThread(50 * 1000 * 1000ULL);
            consoleInit(NULL);
        }
        if (k & HidNpadButton_B)
            break;

        svcSleepThread(16 * 1000 * 1000ULL);
    }

    consoleExit(NULL);
    socketExit();
    appletUnlockExit();
    return 0;
}
