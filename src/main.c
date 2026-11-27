/*
 * 环球电台 / World Radio — live internet radio only.
 *
 * Every station is an HLS live stream: Qingting's 7 s ADTS AAC segments for
 * China, public-broadcaster HLS (ADTS or MPEG-TS) elsewhere. The device
 * downloads one finite segment at a time, decodes it with Helix AAC-LC and
 * queues PCM, so playback survives bobo_http's whole-response buffering.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bobo_app.h"
#include "third_party/helix/aacdec.h"

enum {
  MAX_HITS = 96,
  MAX_NAV = 8,
  MAX_LIVE = 260,
  MAX_LIVE_GROUPS = 24,
  MAX_LIVE_SEG = 10,
};

enum {
  PG_HOME,
  PG_FAVS,
  PG_RECENT,
  PG_LIVE,
  PG_LIVE_ST,
  PG_ABOUT,
};

enum {
  ACT_NONE,
  ACT_FAVS,
  ACT_RECENT,
  ACT_LIVE_FAV,
  ACT_PAGE,
  ACT_LIVE,
  ACT_LIVE_FILTER,
  ACT_LIVE_GROUP,
  ACT_LIVE_STATION,
  ACT_LIVE_PLAY,
  ACT_LIVE_PREV,
  ACT_LIVE_NEXT,
  ACT_LIVE_BACK,
  ACT_ABOUT,
  ACT_LANG_AUTO,
  ACT_LANG_ZH,
  ACT_LANG_EN,
  ACT_RESUME,
};

typedef struct {
  const char *group;
  const char *name;
  const char *sub;
  const char *source;
} live_t;

typedef struct {
  int x, y, w, h;
  int action, a, b;
} hit_t;

static int screen_w, screen_h;
static hit_t hits[MAX_HITS];
static int hit_count;

static int page = PG_HOME;
static int page_arg;
static int nav_page[MAX_NAV], nav_arg[MAX_NAV], nav_list[MAX_NAV], nav_depth;
static int list_first;
static int page_rows = 5, page_total;
static bool dirty = true, first_draw = true, app_should_exit;

static int lang_mode; /* 0 follow system, 1 Chinese, 2 English */
static bool ui_en;

static char fav_live_raw[4096];
static char recent_live_raw[640];
static int live_filter; /* 0 all, 1 China, 2 international */

static char last_ref[192];

static bool live_ok;

/* ---- live radio state ---------------------------------------------------- */
enum {
  LV_CONNECTING,
  LV_PLAYING,
  LV_BUFFERING,
  LV_STOPPED,
  LV_ERROR,
  LV_INTERRUPTED
};

static live_t live[MAX_LIVE];
static int live_count;
static char live_file[40960];
static char live_group_name[MAX_LIVE_GROUPS][48];
static int live_group_first[MAX_LIVE_GROUPS];
static int live_group_n[MAX_LIVE_GROUPS];
static int live_group_count;

static uint8_t *live_net;
static uint8_t *live_ts;
static char live_seg[MAX_LIVE_SEG][512];
static char live_seg_tmp[512];
static int live_seg_count;
static char live_cur[512];
static char live_pl_url[768];
static char live_base[800];
static char live_url[768];
/* The app task stack is 12 KB and the firmware calls made from it
 * (bobo_http/TLS, bobo_refresh, bobo_state_set) need most of that, so big
 * working buffers stay here instead of on the stack (see docs/errors.md). */
static char live_want[512];
static char url_tmp[768];
static char pl_line[512];
static char pl_variant[512];
static char nav_state_buf[160];
static char fav_next_raw[sizeof(fav_live_raw)];
static char recent_next_raw[sizeof(recent_live_raw)];
static char recent_key_buf[160];
static char fav_key_buf[160];
static char draw_line_buf[192];
static char player_line[192];
static char live_stat_buf[64];
static char home_sub_buf[160];
static char about_desc_buf[256];
static int list_scratch[MAX_LIVE];
static int group_scratch[MAX_LIVE_GROUPS];
static HAACDecoder live_dec;
static int16_t live_pcm[4096];
static bool live_audio;
static int live_hz, live_ch;
static int live_state = LV_CONNECTING;
static int live_idx = -1;
static char live_note[128];
static uint32_t live_fetch_ms, live_decode_ms;
static uint32_t live_segs, live_frames, live_underruns;
static uint32_t live_last_ui;
static bool live_priming;
static bool live_prime_resume;
static uint32_t live_prime_ms, live_prime_t0, live_last_prime;
static uint32_t live_intr_t0;
static int live_route = -1; /* sink the open output runs on; -1 unknown */
static uint32_t live_route_ms;
static bool live_wd_armed;
static uint32_t live_wd_t0, live_wd_played;
static int live_fail_streak;
static bool live_exit;
static bool live_quit;
static bool live_stop_req;
static bool live_start_req;
static int live_tune; /* +1 next station, -1 previous, 0 idle */
static uint32_t back_guard_until;

static int px(int n) { return screen_w > 600 ? n * 5 / 4 : n; }

#define APP_VERSION "0.8.6"

static const char *T(const char *zh, const char *en) { return ui_en ? en : zh; }

static int utf8_len(unsigned char c) {
  if (c >= 0xF0) return 4;
  if (c >= 0xE0) return 3;
  if (c >= 0xC0) return 2;
  return 1;
}

static int text_w(int font, const char *s, bool bold) {
  return bobo_text_width(font, s, bold);
}

static void draw_right(int font, int right, int y, const char *s, bool black,
                       bool bold) {
  bobo_draw_text(font, right - text_w(font, s, bold), y, s, black, bold);
}

static void draw_center_col(int font, int y, const char *s, bool black,
                            bool bold) {
  int x = (screen_w - text_w(font, s, bold)) / 2;
  if (x < px(20)) x = px(20);
  bobo_draw_text(font, x, y, s, black, bold);
}

static void draw_spaced(int font, int x, int y, const char *s, int track,
                        bool black, bool bold) {
  while (*s) {
    int n = utf8_len((unsigned char)*s);
    char one[8];
    if (n > 7) n = 7;
    memcpy(one, s, (size_t)n);
    one[n] = '\0';
    bobo_draw_text(font, x, y, one, black, bold);
    x += bobo_text_advance(font, one, bold) + track;
    s += n;
  }
}

static void ellipsize(int font, const char *src, int width, bool bold,
                      char *out, int cap) {
  int n = (int)strlen(src);
  if (n >= cap) {
    n = cap - 1;
    while (n > 0 && ((unsigned char)src[n] & 0xC0) == 0x80) n--;
  }
  memcpy(out, src, (size_t)n);
  out[n] = '\0';
  if (text_w(font, out, bold) <= width) return;
  int dots = text_w(font, "..", bold);
  while (n > 0 && text_w(font, out, bold) + dots > width) {
    n--;
    while (n > 0 && ((unsigned char)out[n] & 0xC0) == 0x80) n--;
    out[n] = '\0';
  }
  if (n + 3 <= cap) strcat(out, "..");
}

static void draw_ellipsized(int font, int x, int y, int width, const char *src,
                            bool black, bool bold) {
  ellipsize(font, src, width, bold, draw_line_buf, sizeof(draw_line_buf));
  bobo_draw_text(font, x, y, draw_line_buf, black, bold);
}

static const char *take_line(const char *p, int font, bool bold, int width,
                             char *out, int cap) {
  while (*p == ' ') p++;
  int len = 0, last_space = -1;
  const char *after_space = NULL;
  out[0] = '\0';
  while (*p) {
    int n = utf8_len((unsigned char)*p);
    if (len + n >= cap) break;
    memcpy(out + len, p, (size_t)n);
    len += n;
    out[len] = '\0';
    bool fits = text_w(font, out, bold) <= width;
    if (*p == ' ' && fits) {
      last_space = len - 1;
      after_space = p + n;
    }
    if (!fits && len > n) {
      if (last_space > 0) {
        out[last_space] = '\0';
        return after_space;
      }
      out[len - n] = '\0';
      return p;
    }
    p += n;
  }
  if (*p && last_space > 0) {
    out[last_space] = '\0';
    return after_space;
  }
  return p;
}

static void append_dots(int font, char *line, int cap, int width, bool bold) {
  int dots = text_w(font, "..", bold);
  int n = (int)strlen(line);
  while (n > 0 && text_w(font, line, bold) + dots > width) {
    n--;
    while (n > 0 && ((unsigned char)line[n] & 0xC0) == 0x80) n--;
    line[n] = '\0';
  }
  if (n + 3 <= cap) strcat(line, "..");
}

static int draw_wrap(int font, const char *text, int x, int y, int width,
                     int max_lines, int line_h, bool bold, bool black) {
  char *line = draw_line_buf;
  const char *p = text;
  int used = 0;
  while (used < max_lines && p && *p) {
    p = take_line(p, font, bold, width, line, sizeof(draw_line_buf));
    if (!line[0]) break;
    if (used == max_lines - 1 && *p)
      append_dots(font, line, sizeof(draw_line_buf), width, bold);
    bobo_draw_text(font, x, y, line, black, bold);
    y += line_h;
    used++;
  }
  return used;
}

static void draw_ruler(int x, int y, int w, int ticks) {
  bobo_draw_line(x, y, x + w, y, 1, true);
  if (ticks < 2) ticks = 2;
  for (int i = 0; i < ticks; i++) {
    int xx = x + w * i / (ticks - 1);
    int h = (i % 5 == 0) ? px(6) : px(3);
    bobo_draw_line(xx, y, xx, y + h, 1, true);
  }
}

/* ---- 1-bit shading ------------------------------------------------------- */

/* mode 0: 50 % checker, mode 1: 25 % stipple. Ink bits are 0. */
static void tone_rect(int x, int y, int w, int h, int mode) {
  static const uint8_t pat50[8] = {0xAA, 0x55, 0xAA, 0x55,
                                   0xAA, 0x55, 0xAA, 0x55};
  static const uint8_t pat25[8] = {0x77, 0xFF, 0xDD, 0xFF,
                                   0x77, 0xFF, 0xDD, 0xFF};
  static uint8_t rows[8][88];
  if (w <= 0 || h <= 0 || x < 0 || y < 0) return;
  if (w > 684) w = 684;
  int bytes = (w + 7) / 8;
  if (bytes > (int)sizeof(rows[0])) return;
  const uint8_t *pat = mode ? pat25 : pat50;
  for (int r = 0; r < 8; r++) {
    memset(rows[r], pat[r], (size_t)bytes);
    int tail = w & 7;
    if (tail) rows[r][bytes - 1] |= (uint8_t)~(0xFFu << (8 - tail));
  }
  for (int yy = 0; yy < h; yy++)
    bobo_draw_bitmap(rows[(y + yy) & 7], w, 1, x, y + yy, true);
}

static void draw_circle(int cx, int cy, int r, int width, bool black) {
  if (r <= 0) return;
  bobo_draw_round_rect(cx - r, cy - r, 2 * r, 2 * r, width, r, black);
}

static void fill_circle(int cx, int cy, int r, bool black) {
  if (r <= 0) return;
  bobo_fill_round_rect(cx - r, cy - r, 2 * r, 2 * r, r, black);
}

/* ---- 1-bit icon set ------------------------------------------------------ */

static const int8_t ICON_COS[24] = {100, 97, 87, 71, 50, 26, 0,  -26, -50, -71,
                                    -87, -97, -100, -97, -87, -71, -50, -26,
                                    0, 26, 50, 71, 87, 97};
static const int8_t ICON_SIN[24] = {0,  26,  50,  71,  87, 97, 100, 97,
                                    87, 71,  50,  26,  0,  -26, -50, -71,
                                    -87, -97, -100, -97, -87, -71, -50, -26};

static int icon_lw(int s) { return s >= 24 ? 2 : 1; }

static void icon_arc(int cx, int cy, int r, int a0, int a1, int width,
                     bool black) {
  if (r < 2) return;
  int steps = (a1 - a0) / 15;
  if (steps < 1) return;
  int i0 = ((a0 % 360) + 360) % 360 / 15;
  int lx = cx + ICON_COS[i0] * r / 100;
  int ly = cy + ICON_SIN[i0] * r / 100;
  for (int i = 1; i <= steps; i++) {
    int ai = (((a0 + i * 15) % 360) + 360) % 360 / 15;
    int nx = cx + ICON_COS[ai] * r / 100;
    int ny = cy + ICON_SIN[ai] * r / 100;
    bobo_draw_line(lx, ly, nx, ny, width, black);
    lx = nx;
    ly = ny;
  }
}

static void fill_poly(const int *xs, const int *ys, int n, bool black) {
  if (n < 3) return;
  int miny = ys[0], maxy = ys[0];
  for (int i = 1; i < n; i++) {
    if (ys[i] < miny) miny = ys[i];
    if (ys[i] > maxy) maxy = ys[i];
  }
  if (maxy - miny > 512) return;
  for (int y = miny; y <= maxy; y++) {
    int sx[16], m = 0;
    for (int i = 0; i < n && m < 16; i++) {
      int j = (i + 1) % n;
      int y1 = ys[i], y2 = ys[j];
      if (y1 == y2) continue;
      if ((y >= y1 && y < y2) || (y >= y2 && y < y1))
        sx[m++] = xs[i] + (y - y1) * (xs[j] - xs[i]) / (y2 - y1);
    }
    for (int i = 0; i < m; i++)
      for (int j = i + 1; j < m; j++)
        if (sx[j] < sx[i]) {
          int t = sx[i];
          sx[i] = sx[j];
          sx[j] = t;
        }
    for (int i = 0; i + 1 < m; i += 2)
      if (sx[i + 1] > sx[i])
        bobo_fill_rect(sx[i], y, sx[i + 1] - sx[i], 1, black);
  }
}

static void icon_play(int x, int y, int s, bool black) {
  int xs[3] = {x + s * 28 / 100, x + s * 78 / 100, x + s * 28 / 100};
  int ys[3] = {y + s * 16 / 100, y + s / 2, y + s * 84 / 100};
  fill_poly(xs, ys, 3, black);
}

static void icon_stop(int x, int y, int s, bool black) {
  bobo_fill_round_rect(x + s * 22 / 100, y + s * 22 / 100, s * 56 / 100,
                       s * 56 / 100, s / 10, black);
}

/* Skip track: bar plus triangle, pointing forward or back. */
static void icon_skip(int x, int y, int s, bool black, bool forward) {
  int bar = s / 6;
  if (bar < 2) bar = 2;
  int tri = s * 3 / 5;
  if (tri < 3) tri = 3;
  if (forward) {
    bobo_fill_rect(x + s - bar, y, bar, s, black);
    int xs[3] = {x + s - bar - tri, x + s - bar - tri, x + s - bar};
    int ys[3] = {y, y + s, y + s / 2};
    fill_poly(xs, ys, 3, black);
  } else {
    bobo_fill_rect(x, y, bar, s, black);
    int xs[3] = {x + bar + tri, x + bar + tri, x + bar};
    int ys[3] = {y, y + s, y + s / 2};
    fill_poly(xs, ys, 3, black);
  }
}

static void icon_wave(int x, int y, int s, bool black) {
  static const int bars[4] = {35, 82, 100, 55};
  int bw = s / 6;
  if (bw < 1) bw = 1;
  int gap = (s - 4 * bw) / 3;
  if (gap < 1) gap = 1;
  int total = 4 * bw + 3 * gap;
  int sx = x + (s - total) / 2;
  int base = y + s * 90 / 100;
  for (int i = 0; i < 4; i++) {
    int h = s * bars[i] / 100;
    if (h < 1) h = 1;
    bobo_fill_rect(sx + i * (bw + gap), base - h, bw, h, black);
  }
}

static void icon_clock(int x, int y, int s, bool black) {
  int cx = x + s / 2, cy = y + s / 2, r = s * 42 / 100;
  if (r < 2) return;
  fill_circle(cx, cy, r, black);
  bobo_draw_line(cx, cy, cx, cy - r * 55 / 100, r >= 7 ? 2 : 1, !black);
  bobo_draw_line(cx, cy, cx + r * 40 / 100, cy, r >= 7 ? 2 : 1, !black);
}

static void icon_star(int x, int y, int s, bool black) {
  int cx = x + s / 2, cy = y + s / 2;
  int ro = s * 46 / 100, ri = s * 19 / 100;
  static const int ox[5] = {0, 95, 59, -59, -95};
  static const int oy[5] = {-100, -31, 81, 81, -31};
  static const int ix[5] = {59, 95, 0, -95, -59};
  static const int iy[5] = {-81, 31, 100, 31, -81};
  int xs[10], ys[10];
  for (int i = 0; i < 5; i++) {
    xs[i * 2] = cx + ox[i] * ro / 100;
    ys[i * 2] = cy + oy[i] * ro / 100;
    xs[i * 2 + 1] = cx + ix[i] * ri / 100;
    ys[i * 2 + 1] = cy + iy[i] * ri / 100;
  }
  fill_poly(xs, ys, 10, black);
}

static void icon_star_outline(int x, int y, int s, bool black) {
  icon_star(x, y, s, black);
  int inner = s * 56 / 100;
  if (inner < 2) return;
  icon_star(x + (s - inner) / 2, y + (s - inner) / 2, inner, !black);
}

static void icon_headphones(int x, int y, int s, bool black) {
  int cx = x + s / 2;
  icon_arc(cx, y + s * 50 / 100, s * 38 / 100, 180, 360, icon_lw(s), black);
  bobo_fill_round_rect(x + s * 6 / 100, y + s * 46 / 100, s * 20 / 100,
                       s * 38 / 100, s * 7 / 100, black);
  bobo_fill_round_rect(x + s * 74 / 100, y + s * 46 / 100, s * 20 / 100,
                       s * 38 / 100, s * 7 / 100, black);
}

static void icon_broadcast(int x, int y, int s, bool black) {
  int cx = x + s / 2;
  int dy = y + s * 26 / 100;
  fill_circle(cx, dy, s * 8 / 100, black);
  icon_arc(cx, dy, s * 22 / 100, 210, 330, 1, black);
  icon_arc(cx, dy, s * 34 / 100, 210, 330, 1, black);
  int top = y + s * 40 / 100, bot = y + s * 92 / 100;
  bobo_draw_line(cx, top, cx - s * 20 / 100, bot, icon_lw(s), black);
  bobo_draw_line(cx, top, cx + s * 20 / 100, bot, icon_lw(s), black);
  bobo_draw_line(cx - s * 8 / 100, y + s * 68 / 100, cx + s * 8 / 100,
                 y + s * 68 / 100, 1, black);
  bobo_draw_line(cx - s * 20 / 100, bot, cx + s * 20 / 100, bot, 1, black);
}

static void icon_antenna(int x, int y, int s, bool black) {
  int cx = x + s / 2;
  int top = y + s * 14 / 100, bot = y + s * 90 / 100;
  bobo_draw_line(cx, top, cx, bot, icon_lw(s), black);
  bobo_draw_line(cx - s * 30 / 100, bot, cx, y + s * 36 / 100, 1, black);
  bobo_draw_line(cx + s * 30 / 100, bot, cx, y + s * 36 / 100, 1, black);
  icon_arc(cx, top, s * 20 / 100, 200, 340, 1, black);
  icon_arc(cx, top, s * 34 / 100, 200, 340, 1, black);
}

static void icon_globe(int x, int y, int s, bool black) {
  int cx = x + s / 2, cy = y + s / 2, r = s * 40 / 100;
  if (r < 2) return;
  draw_circle(cx, cy, r, icon_lw(s), black);
  bobo_draw_line(cx - r, cy, cx + r, cy, 1, black);
  bobo_draw_line(cx, cy - r, cx, cy + r, 1, black);
  int dy = r / 2, hw = r * 87 / 100;
  bobo_draw_line(cx - hw, cy - dy, cx + hw, cy - dy, 1, black);
  bobo_draw_line(cx - hw, cy + dy, cx + hw, cy + dy, 1, black);
}

static void icon_gh(int x, int y, int s, bool black) {
  int cx = x + s / 2;
  int cy = y + s * 60 / 100;
  int r = s * 28 / 100;
  /* Octocat head: two pointed ears flanking a round face. */
  int lx[3] = {cx - r * 95 / 100, cx - r * 95 / 100, cx - r * 28 / 100};
  int ly[3] = {cy - r * 30 / 100, cy - r * 150 / 100, cy - r * 92 / 100};
  fill_poly(lx, ly, 3, black);
  int rx[3] = {cx + r * 95 / 100, cx + r * 95 / 100, cx + r * 28 / 100};
  int ry[3] = {cy - r * 30 / 100, cy - r * 150 / 100, cy - r * 92 / 100};
  fill_poly(rx, ry, 3, black);
  fill_circle(cx, cy, r, black);
  int er = s * 6 / 100;
  if (er < 1) er = 1;
  fill_circle(cx - r * 40 / 100, cy - r * 8 / 100, er, !black);
  fill_circle(cx + r * 40 / 100, cy - r * 8 / 100, er, !black);
}

static void draw_screw(int cx, int cy, int r) {
  draw_circle(cx, cy, r, 1, true);
  bobo_draw_line(cx - r + 2, cy + r - 2, cx + r - 2, cy - r + 2, 1, true);
}

static void draw_knob(int cx, int cy, int r) {
  static const int8_t ux[12] = {0,  50,  87, 100, 87,  50,
                                0, -50, -87, -100, -87, -50};
  static const int8_t uy[12] = {-100, -87, -50, 0, 50,  87,
                                100,  87,  50, 0, -50, -87};
  draw_circle(cx, cy, r, 2, true);
  draw_circle(cx, cy, r - 3, 1, true);
  for (int i = 0; i < 12; i++) {
    int x1 = cx + ux[i] * (r - 2) / 100;
    int y1 = cy + uy[i] * (r - 2) / 100;
    int x2 = cx + ux[i] * (r + 2) / 100;
    int y2 = cy + uy[i] * (r + 2) / 100;
    bobo_draw_line(x1, y1, x2, y2, 1, true);
  }
  bobo_draw_line(cx, cy, cx + ux[1] * (r - 5) / 100,
                 cy + uy[1] * (r - 5) / 100, 2, true);
  fill_circle(cx, cy, 2, true);
}

static void draw_tuner(int x, int y, int w, int h, int channel, int total) {
  bobo_draw_rect(x, y, w, h, 1, true);
  bobo_draw_rect(x + 2, y + 2, w - 4, h - 4, 1, true);
  int ticks = 13;
  for (int i = 0; i < ticks; i++) {
    int tx = x + 6 + (w - 12) * i / (ticks - 1);
    int th = (i % 3 == 0) ? px(7) : px(4);
    bobo_draw_line(tx, y + h - 6 - th, tx, y + h - 6, 1, true);
  }
  int slot = total > 1 ? (channel - 1) * 100 / (total - 1) : 50;
  if (slot < 0) slot = 0;
  if (slot > 100) slot = 100;
  int nx = x + 8 + (w - 16) * slot / 100;
  bobo_draw_line(nx, y + px(18), nx, y + h - 8, 2, true);
  char buf[16];
  snprintf(buf, sizeof(buf), "CH %02d", channel);
  bobo_draw_text(BOBO_FONT_SMALL, x + 5, y + 4, buf, true, true);
}

/* Isometric-flavoured front panel: white case, dither shadows, knurled knobs,
 * screws and a needle tuner. `case_y`/`bh` place the case directly so the
 * player can size the receiver block to the drawing instead of a fixed card. */
static void draw_radio_art(int cx, int case_y, int w, int bh, bool on_air,
                           int channel, int total) {
  int bw = w * 80 / 100;
  if (bw > px(430)) bw = px(430);
  int bx = cx - bw / 2;
  int by = case_y;
  int r = px(13);

  tone_rect(bx + px(7), by + px(7), bw, bh, 0);

  int ax0 = bx + bw - px(44), ay0 = by;
  int ax1 = ax0 + px(30), ay1 = by - px(34);
  bobo_draw_line(ax0, ay0, ax1, ay1, 1, true);
  bobo_draw_line(ax0 + 4, ay0, ax1 + 4, ay1, 1, true);
  bobo_draw_line(ax1 - 3, ay1 + px(4), ax1 + 7, ay1 - px(4), 1, true);
  fill_circle(ax1 + 2, ay1 - 1, px(3), true);
  draw_circle(ax0, ay0, px(5), 1, true);
  fill_circle(ax0, ay0, px(2), true);

  bobo_fill_round_rect(bx + bw * 13 / 100, by + bh, px(44), px(8), 2, true);
  bobo_fill_round_rect(bx + bw * 67 / 100, by + bh, px(44), px(8), 2, true);

  bobo_fill_round_rect(bx, by, bw, bh, r, false);
  bobo_draw_round_rect(bx, by, bw, bh, 2, r, true);
  bobo_draw_round_rect(bx + px(4), by + px(4), bw - px(8), bh - px(8), 1,
                       r - px(3), true);
  draw_screw(bx + px(11), by + px(11), px(3));
  draw_screw(bx + bw - px(11), by + px(11), px(3));
  draw_screw(bx + px(11), by + bh - px(11), px(3));
  draw_screw(bx + bw - px(11), by + bh - px(11), px(3));

  draw_spaced(BOBO_FONT_SMALL, bx + px(22), by + px(13), "WORLD RADIO", 1, true,
              true);
  fill_circle(bx + bw - px(46), by + px(20), px(3), on_air);
  draw_circle(bx + bw - px(46), by + px(20), px(3), 1, true);

  int gx = bx + px(16), gy = by + px(36);
  int gw = bw * 42 / 100, gh = bh - px(52);
  bobo_fill_round_rect(gx, gy, gw, gh, px(5), false);
  bobo_draw_round_rect(gx, gy, gw, gh, 1, px(5), true);
  int pitch = px(7);
  int cols = (gw - px(12)) / pitch;
  int rows = (gh - px(12)) / pitch;
  for (int ry = 0; ry <= rows; ry++)
    for (int cxx = 0; cxx <= cols; cxx++)
      bobo_fill_round_rect(gx + px(5) + cxx * pitch,
                           gy + px(5) + ry * pitch, px(2), px(2), 1, true);
  tone_rect(gx + 1, gy + 1, gw - 2, px(4), 1);

  int tx = bx + bw * 53 / 100, ty = by + px(34);
  int tw = bw * 41 / 100, th = bh * 34 / 100;
  draw_tuner(tx, ty, tw, th, channel, total);

  int ky = by + bh - px(40);
  int k1 = tx + tw / 4, k2 = tx + tw * 3 / 4;
  draw_knob(k1, ky, px(13));
  draw_knob(k2, ky, px(13));
  bobo_draw_text(BOBO_FONT_SMALL, k1 - px(9), ky + px(16), "VOL", true, false);
  bobo_draw_text(BOBO_FONT_SMALL, k2 - px(12), ky + px(16), "TUNE", true,
                 false);
  for (int i = 0; i < 4; i++)
    bobo_draw_line(gx, by + bh - px(12) + i * px(3), gx + gw * 2 / 3,
                   by + bh - px(12) + i * px(3), 1, true);
}

static int fast_refreshes;

static void present(int mode) {
  if (mode == BOBO_REFRESH_FAST && ++fast_refreshes >= 20) {
    mode = BOBO_REFRESH_HALF;
    fast_refreshes = 0;
  } else if (mode == BOBO_REFRESH_HALF) {
    fast_refreshes = 0;
  }
  bobo_refresh(mode);
}

typedef struct {
  bool valid;
  int hour, min;
  int year, month, day, wday; /* wday: 0 = Sunday */
} wall_clock_t;

static void civil_from_days(int64_t z, int *year, int *month, int *day) {
  z += 719468;
  int64_t era = (z >= 0 ? z : z - 146096) / 146097;
  unsigned doe = (unsigned)(z - era * 146097);
  unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  int64_t y = (int64_t)yoe + era * 400;
  unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  unsigned mp = (5 * doy + 2) / 153;
  unsigned d = doy - (153 * mp + 2) / 5 + 1;
  unsigned m = mp < 10 ? mp + 3 : mp - 9;
  *year = (int)(y + (m <= 2));
  *month = (int)m;
  *day = (int)d;
}

static void wall_clock(wall_clock_t *c) {
  int64_t t = bobo_time();
  if (t <= 0) {
    c->valid = false;
    c->hour = c->min = 0;
    c->year = c->month = c->day = c->wday = 0;
    return;
  }
  int64_t local = t + bobo_tz_offset();
  if (local < 0) local = 0;
  c->valid = true;
  int sec = (int)(local % 86400);
  c->hour = sec / 3600;
  c->min = (sec / 60) % 60;
  int64_t days = local / 86400;
  civil_from_days(days, &c->year, &c->month, &c->day);
  c->wday = (int)((days + 4) % 7);
  if (c->wday < 0) c->wday += 7;
}

static void clock_hm(const wall_clock_t *c, char *out, int cap) {
  if (!c->valid)
    snprintf(out, (size_t)cap, "--:--");
  else
    snprintf(out, (size_t)cap, "%02d:%02d", c->hour, c->min);
}

static void clock_date(const wall_clock_t *c, char *out, int cap) {
  static const char *ZH[7] = {"周日", "周一", "周二", "周三",
                              "周四", "周五", "周六"};
  static const char *EN[7] = {"SUN", "MON", "TUE", "WED",
                              "THU", "FRI", "SAT"};
  if (!c->valid) {
    snprintf(out, (size_t)cap, "----.--.--");
    return;
  }
  snprintf(out, (size_t)cap, "%04d.%02d.%02d %s", c->year, c->month, c->day,
           ui_en ? EN[c->wday] : ZH[c->wday]);
}

static int split_tabs(char *line, char **out, int max) {
  int n = 0;
  char *p = line;
  while (n < max) {
    out[n++] = p;
    char *t = strchr(p, '\t');
    if (!t) break;
    *t = '\0';
    p = t + 1;
  }
  return n;
}

static bool load_live(void) {
  int n = bobo_file_read("live.tsv", live_file, sizeof(live_file) - 1);
  if (n <= 0) return false;
  live_file[n] = '\0';
  live_count = 0;
  live_group_count = 0;
  char *p = live_file;
  while (*p && live_count < MAX_LIVE) {
    char *nl = strchr(p, '\n');
    if (nl) *nl = '\0';
    if (*p && *p != '#') {
      char *f[4];
      int nf = split_tabs(p, f, 4);
      if (nf >= 4) {
        live_t *l = &live[live_count];
        l->group = f[0];
        l->name = f[1];
        l->sub = f[2];
        l->source = f[3];
        if (live_group_count == 0 ||
            strcmp(live_group_name[live_group_count - 1], f[0])) {
          if (live_group_count >= MAX_LIVE_GROUPS) break;
          strncpy(live_group_name[live_group_count], f[0],
                  sizeof(live_group_name[0]) - 1);
          live_group_name[live_group_count][sizeof(live_group_name[0]) - 1] =
              '\0';
          live_group_first[live_group_count] = live_count;
          live_group_n[live_group_count] = 0;
          live_group_count++;
        }
        live_group_n[live_group_count - 1]++;
        live_count++;
      }
    }
    if (!nl) break;
    p = nl + 1;
  }
  return live_count > 0;
}

static const char *live_group_en(const char *zh) {
  static const struct {
    const char *zh, *en;
  } tab[] = {
      {"中国 · 北京", "BEIJING"},
      {"中国 · 上海", "SHANGHAI"},
      {"中国 · 浙江", "ZHEJIANG / HANGZHOU"},
      {"中国 · 江苏", "JIANGSU"},
      {"中国 · 广东", "GUANGDONG"},
      {"中国 · 华东", "EAST CHINA"},
      {"中国 · 华北", "NORTH CHINA"},
      {"中国 · 东北", "NORTHEAST CHINA"},
      {"中国 · 华中", "CENTRAL CHINA"},
      {"中国 · 华南", "SOUTH CHINA"},
      {"中国 · 西南", "SOUTHWEST CHINA"},
      {"中国 · 西北", "NORTHWEST CHINA"},
      {"中国 · 港澳台", "HONG KONG / TAIWAN"},
      {"国际 · 亚洲", "ASIA"},
      {"国际 · 欧洲", "EUROPE"},
      {"国际 · 美洲", "AMERICAS"},
      {"国际 · 大洋洲与非洲", "OCEANIA / AFRICA"},
  };
  for (size_t i = 0; i < sizeof(tab) / sizeof(tab[0]); i++)
    if (!strcmp(tab[i].zh, zh)) return tab[i].en;
  return "";
}

static bool live_group_is_cn(const char *group) {
  return !strncmp(group, "中国", 6);
}

static int live_group_stations(int filter) {
  int n = 0;
  for (int g = 0; g < live_group_count; g++) {
    bool cn = live_group_is_cn(live_group_name[g]);
    if (filter == 1 && !cn) continue;
    if (filter == 2 && cn) continue;
    n += live_group_n[g];
  }
  return n;
}

static void live_key(int idx, char *out, size_t cap) {
  snprintf(out, cap, "%s|%s", live[idx].group, live[idx].name);
}

static bool key_in_list(const char *list, const char *key) {
  size_t n = strlen(key);
  const char *p = list;
  while (*p) {
    const char *e = strchr(p, '\n');
    size_t len = e ? (size_t)(e - p) : strlen(p);
    if (len == n && !strncmp(p, key, n)) return true;
    if (!e) break;
    p = e + 1;
  }
  return false;
}

static int live_key_index(const char *key) {
  char buf[160];
  for (int i = 0; i < live_count; i++) {
    live_key(i, buf, sizeof(buf));
    if (!strcmp(buf, key)) return i;
  }
  return -1;
}

static bool live_is_fav(int idx) {
  if (idx < 0 || idx >= live_count) return false;
  live_key(idx, fav_key_buf, sizeof(fav_key_buf));
  return key_in_list(fav_live_raw, fav_key_buf);
}

static int live_fav_indices(int *out, int max) {
  int count = 0;
  for (int i = 0; i < live_count && count < max; i++)
    if (live_is_fav(i)) out[count++] = i;
  return count;
}

static void live_fav_toggle(int idx) {
  if (idx < 0 || idx >= live_count) return;
  char *key = fav_key_buf;
  live_key(idx, key, sizeof(fav_key_buf));
  if (key_in_list(fav_live_raw, key)) {
    char *next = fav_next_raw;
    int n = 0;
    next[0] = '\0';
    const char *p = fav_live_raw;
    while (*p) {
      const char *e = strchr(p, '\n');
      size_t len = e ? (size_t)(e - p) : strlen(p);
      if (len > 0 && !(len == strlen(key) && !strncmp(p, key, len)) &&
          n + (int)len + 2 < (int)sizeof(fav_next_raw)) {
        memcpy(next + n, p, len);
        n += (int)len;
        next[n++] = '\n';
        next[n] = '\0';
      }
      if (!e) break;
      p = e + 1;
    }
    snprintf(fav_live_raw, sizeof(fav_live_raw), "%s", next);
  } else {
    size_t used = strlen(fav_live_raw);
    size_t need = strlen(key) + (used ? 1 : 0);
    if (used + need + 1 < sizeof(fav_live_raw))
      snprintf(fav_live_raw + used, sizeof(fav_live_raw) - used, "%s%s",
               used ? "\n" : "", key);
  }
  bobo_state_set("fav_live", fav_live_raw);
  bobo_log("live favorite %s: %s", key,
           key_in_list(fav_live_raw, key) ? "on" : "off");
}

static void recent_live_add(int idx) {
  if (idx < 0 || idx >= live_count) return;
  char *key = recent_key_buf;
  live_key(idx, key, sizeof(recent_key_buf));
  size_t keylen = strlen(key);
  char *next = recent_next_raw;
  int n = snprintf(next, sizeof(recent_next_raw), "%s\n", key);
  if (n < 0) n = 0;
  int kept = 0;
  const char *p = recent_live_raw;
  while (*p && kept < 7) {
    const char *e = strchr(p, '\n');
    if (!e) break;
    size_t len = (size_t)(e - p);
    bool same = len == keylen && !strncmp(p, key, keylen);
    if (len > 0 && !same && n + (int)len + 2 < (int)sizeof(recent_next_raw)) {
      memcpy(next + n, p, len);
      n += (int)len;
      next[n++] = '\n';
      next[n] = '\0';
      kept++;
    }
    p = e + 1;
  }
  snprintf(recent_live_raw, sizeof(recent_live_raw), "%s", next);
  bobo_state_set("recent_live", recent_live_raw);
}

static int recent_live_indices(int *out, int max) {
  int count = 0;
  const char *p = recent_live_raw;
  while (*p && count < max) {
    const char *e = strchr(p, '\n');
    if (!e) break;
    size_t len = (size_t)(e - p);
    if (len > 0 && len < sizeof(recent_key_buf)) {
      memcpy(recent_key_buf, p, len);
      recent_key_buf[len] = '\0';
      int idx = live_key_index(recent_key_buf);
      if (idx >= 0) out[count++] = idx;
    }
    p = e + 1;
  }
  return count;
}

static void load_state(void) {
  if (bobo_state_get("fav_live", fav_live_raw, sizeof(fav_live_raw)) < 0)
    fav_live_raw[0] = '\0';
  if (bobo_state_get("recent_live", recent_live_raw,
                     sizeof(recent_live_raw)) < 0)
    recent_live_raw[0] = '\0';
  char tmp[16];
  if (bobo_state_get("live_filter", tmp, sizeof(tmp)) > 0) {
    int f = atoi(tmp);
    if (f >= 0 && f <= 2) live_filter = f;
  }
}

/* API 6 has no locale call. "Follow the system" therefore probes a few
 * plausible state keys once (the OS does not currently publish any) and
 * otherwise stays on the app's primary language. */
static bool system_prefers_english(void) {
  char buf[24];
  static const char *keys[] = {"system_lang", "ui_lang", "locale", "language"};
  for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
    if (bobo_state_get(keys[i], buf, sizeof(buf)) <= 0 || !buf[0]) continue;
    if (buf[0] == 'e' || buf[0] == 'E') return true;
    if (buf[0] == 'z' || buf[0] == 'Z') return false;
  }
  return false;
}

static void apply_language(void) {
  ui_en = lang_mode == 2 || (lang_mode == 0 && system_prefers_english());
}

static void load_language(void) {
  char v[16];
  lang_mode = 0;
  if (bobo_state_get("lang_mode", v, sizeof(v)) > 0) {
    if (!strcmp(v, "zh"))
      lang_mode = 1;
    else if (!strcmp(v, "en"))
      lang_mode = 2;
  }
  apply_language();
}

static void set_language_mode(int mode) {
  lang_mode = mode;
  bobo_state_set("lang_mode", mode == 1 ? "zh" : mode == 2 ? "en" : "auto");
  apply_language();
}

/* ---- last played / player resume ---------------------------------------- */

static int live_source_index(const char *src) {
  if (!src || !src[0]) return -1;
  for (int i = 0; i < live_count; i++)
    if (!strcmp(live[i].source, src)) return i;
  return -1;
}

static int live_group_of(int idx) {
  for (int g = 0; g < live_group_count; g++)
    if (idx >= live_group_first[g] &&
        idx < live_group_first[g] + live_group_n[g])
      return g;
  return 0;
}

static void load_last(void) {
  char ref[192];
  last_ref[0] = '\0';
  if (bobo_state_get("last_ref", ref, sizeof(ref)) > 0)
    snprintf(last_ref, sizeof(last_ref), "%s", ref);
}

static void remember_live(int idx) {
  if (idx < 0 || idx >= live_count) return;
  snprintf(last_ref, sizeof(last_ref), "%s", live[idx].source);
  bobo_state_set("last_kind", "live");
  bobo_state_set("last_ref", last_ref);
}

static int last_live_index(void) { return live_source_index(last_ref); }

static int resume_live_index(void) {
  char ref[192];
  if (bobo_state_get("resume_live", ref, sizeof(ref)) <= 0) return -1;
  return live_source_index(ref);
}

static void add_hit(int x, int y, int w, int h, int action, int a, int b) {
  if (hit_count >= MAX_HITS) return;
  hit_t *ht = &hits[hit_count++];
  ht->x = x;
  ht->y = y;
  ht->w = w;
  ht->h = h;
  ht->action = action;
  ht->a = a;
  ht->b = b;
}

static bool inside(const hit_t *h, int x, int y) {
  return x >= h->x && x < h->x + h->w && y >= h->y && y < h->y + h->h;
}

static void row_basics(int x, int y, int w, int h, int action, int a) {
  bobo_draw_line(x, y + h - 1, x + w, y + h - 1, 1, true);
  add_hit(x, y, w, h, action, a, 0);
}

static void draw_pager(int action, int first, int rows, int total, int y) {
  if (total <= 0 || rows <= 0) return;
  int pages = (total + rows - 1) / rows;
  if (pages <= 1) return;
  int side = px(24);
  int w = screen_w - 2 * side;
  int h = px(48);
  bobo_draw_line(side, y, side + w, y, 1, true);
  int current = first / rows + 1;
  char mid[32];
  snprintf(mid, sizeof(mid), "%d / %d", current, pages);
  draw_center_col(BOBO_FONT_SMALL, y + px(14), mid, true, false);
  if (first > 0) {
    bobo_draw_text(BOBO_FONT_SMALL, side, y + px(14), T("< 上一页", "< PREV"),
                   true, true);
    add_hit(side, y, w / 3, h, action, -1, 0);
  }
  if (first + rows < total) {
    const char *label = T("下一页 >", "NEXT >");
    bobo_draw_text(BOBO_FONT_SMALL,
                   side + w - text_w(BOBO_FONT_SMALL, label, true),
                   y + px(14), label, true, true);
    add_hit(side + w - w / 3, y, w / 3, h, action, 1, 0);
  }
}

static void draw_live_row(const live_t *l, int idx, int x, int y, int w, int h,
                          bool star) {
  const int lh = bobo_line_height(BOBO_FONT_SMALL);
  draw_ellipsized(BOBO_FONT_BODY, x, y + px(8), w - px(star ? 40 : 8), l->name,
                  true, true);
  draw_ellipsized(BOBO_FONT_SMALL, x, y + h - lh - px(11),
                  w - px(star ? 96 : 64), l->sub, true, false);
  const char *live_lbl = T("直播 >", "LIVE >");
  int llw = text_w(BOBO_FONT_SMALL, live_lbl, false);
  int right = x + w - (star ? px(34) : 0);
  draw_right(BOBO_FONT_SMALL, right, y + h - lh - px(11), live_lbl, true, false);
  icon_play(right - llw - px(15), y + h - lh - px(10), px(10), true);
  row_basics(x, y, w, h, ACT_LIVE_STATION, idx);
  if (star) {
    int s = px(15);
    int sx = x + w - s;
    int sy = y + (h - s) / 2;
    if (live_is_fav(idx))
      icon_star(sx, sy, s, true);
    else
      icon_star_outline(sx, sy, s, true);
    add_hit(x + w - px(36), y, px(36), h, ACT_LIVE_FAV, idx, 0);
  }
}

static void draw_empty_box(int y, int h, const char *l1, const char *l2,
                           const char *l3) {
  int side = px(24);
  int w = screen_w - 2 * side;
  int inner = w - px(36);
  bobo_draw_rect(side, y, w, h, 1, true);
  int ty = y + px(20);
  if (l1) {
    bobo_draw_text(BOBO_FONT_BODY, side + px(18), ty, l1, true, true);
    ty += px(30);
  }
  if (l2) {
    draw_ellipsized(BOBO_FONT_SMALL, side + px(18), ty, inner, l2, true, false);
    ty += px(20);
  }
  if (l3)
    draw_ellipsized(BOBO_FONT_SMALL, side + px(18), ty, inner, l3, true, false);
}

static void group_short_name(const char *group, char *out, size_t cap);
static const char *group_en_short(const char *zh);

static void draw_home(void) {
  const int side = px(24);
  const int w = screen_w - 2 * side;
  const int slh = bobo_line_height(BOBO_FONT_SMALL);
  const int tlh = bobo_line_height(BOBO_FONT_TITLE);
  int y = bobo_draw_header(T("环球电台", "World Radio")) + px(16);

  /* Masthead: typography and rules instead of a black plate. */
  draw_spaced(BOBO_FONT_TITLE, side, y, "WORLD RADIO", px(3), true, true);
  const char *about_lbl = T("关于 >", "ABOUT >");
  int about_w = text_w(BOBO_FONT_SMALL, about_lbl, false);
  bobo_draw_text(BOBO_FONT_SMALL, side + w - about_w, y + (tlh - slh) / 2,
                 about_lbl, true, true);
  add_hit(side + w - about_w - px(12), y - px(8), about_w + px(12), px(34),
          ACT_ABOUT, 0, 0);
  y += tlh + px(10);

  draw_spaced(BOBO_FONT_SMALL, side, y, "LIVE INTERNET RADIO", 1, true, true);
  draw_right(BOBO_FONT_SMALL, side + w, y, "AAC · HLS", true, false);
  y += px(22);
  char meta[64];
  snprintf(meta, sizeof(meta),
           T("%d 台 · %d 个地区分组", "%d STATIONS · %d REGIONS"), live_count,
           live_group_count);
  bobo_draw_text(BOBO_FONT_SMALL, side, y, meta, true, false);
  y += px(26);
  bobo_draw_line(side, y, side + w, y, 1, true);
  y += px(18);

  if (live_count > 0) {
    int hero_h = px(62);
    icon_broadcast(side + px(1), y + (hero_h - px(34)) / 2, px(34), true);
    draw_ellipsized(BOBO_FONT_TITLE, side + px(50), y + px(7),
                    w - px(50) - px(96), T("直播电台", "Live radio"), true,
                    true);
    draw_spaced(BOBO_FONT_SMALL, side + px(50), y + px(37),
                "LIVE RADIO / WORLD", 1, true, false);
    char lc[24];
    snprintf(lc, sizeof(lc), T("%d 台 >", "%d >"), live_count);
    draw_right(BOBO_FONT_SMALL, side + w, y + px(22), lc, true, true);
    add_hit(side, y, w, hero_h, ACT_LIVE, 0, 0);
    bobo_draw_line(side, y + hero_h, side + w, y + hero_h, 1, true);
    y += hero_h + px(20);
  }

  int last_live = last_live_index();
  if (last_live >= 0) {
    int ch = px(58);
    bobo_draw_rect(side, y, w, ch, 1, true);
    int block = px(44);
    bobo_fill_rect(side + px(1), y + px(1), block, ch - px(2), true);
    icon_headphones(side + px(1) + (block - px(26)) / 2, y + (ch - px(26)) / 2,
                    px(26), false);
    draw_ellipsized(BOBO_FONT_BODY, side + block + px(16), y + px(9),
                    w - block - px(94), live[last_live].name, true, true);
    char *resume_sub = home_sub_buf;
    snprintf(resume_sub, sizeof(home_sub_buf), "%s · %s",
             ui_en ? live_group_en(live[last_live].group)
                   : live[last_live].group,
             T("上次收听", "LAST PLAYED"));
    draw_ellipsized(BOBO_FONT_SMALL, side + block + px(16), y + px(35),
                    w - block - px(94), resume_sub, true, false);
    draw_right(BOBO_FONT_SMALL, side + w - px(14), y + px(21),
               T("继续 >", "RESUME >"), true, true);
    add_hit(side, y, w, ch, ACT_RESUME, 0, 0);
    y += ch + px(22);
  }

  draw_spaced(BOBO_FONT_SMALL, side, y, "REGIONS", px(2), true, true);
  draw_right(BOBO_FONT_SMALL, side + w, y, T("全部 >", "ALL >"), true, false);
  add_hit(side + w - px(60), y - px(4), px(60), px(22), ACT_LIVE, 0, 0);
  y += px(24);

  const char *flabels[3] = {T("全部", "ALL"), T("中国", "CHINA"),
                            T("国际", "WORLD")};
  int fcounts[3] = {live_count, live_group_stations(1), live_group_stations(2)};
  int gap = px(8);
  int bw = (w - 2 * gap) / 3;
  int bh = px(44);
  for (int i = 0; i < 3; i++) {
    int bx = side + i * (bw + gap);
    bool sel = live_filter == i;
    char label[48];
    snprintf(label, sizeof(label), "%s %d", flabels[i], fcounts[i]);
    if (sel)
      bobo_fill_round_rect(bx, y, bw, bh, px(3), true);
    else
      bobo_draw_round_rect(bx, y, bw, bh, 1, px(3), true);
    bobo_draw_text(BOBO_FONT_SMALL,
                   bx + (bw - text_w(BOBO_FONT_SMALL, label, true)) / 2,
                   y + (bh - slh) / 2, label, !sel, true);
    add_hit(bx, y, bw, bh, ACT_LIVE_FILTER, i, 0);
  }
  y += bh + px(16);

  if (live_filter != 2) {
    int cn_groups = 0;
    for (int g = 0; g < live_group_count; g++)
      if (live_group_is_cn(live_group_name[g])) cn_groups++;
    int cc_h = px(58);
    bobo_draw_rect(side, y, w, cc_h, 1, true);
    icon_globe(side + px(13), y + (cc_h - px(22)) / 2, px(22), true);
    bobo_draw_text(BOBO_FONT_BODY, side + px(47), y + px(9),
                   T("中国", "China"), true, true);
    char cnsub[48];
    snprintf(cnsub, sizeof(cnsub), T("CHINA · %d 个地区", "CHINA · %d REGIONS"),
             cn_groups);
    draw_spaced(BOBO_FONT_SMALL, side + px(47), y + px(35), cnsub, 1, true,
                false);
    char cnc[24];
    snprintf(cnc, sizeof(cnc), T("%d 台", "%d STATIONS"),
             live_group_stations(1));
    draw_right(BOBO_FONT_BODY, side + w - px(14),
               y + (cc_h - bobo_line_height(BOBO_FONT_BODY)) / 2, cnc, true,
               true);
    add_hit(side, y, w, cc_h, ACT_LIVE_GROUP, -1, 0);
    y += cc_h + px(10);
  }

  int qidx[MAX_LIVE_GROUPS], qn = 0;
  for (int g = 0; g < live_group_count; g++) {
    bool cn = live_group_is_cn(live_group_name[g]);
    if (live_filter == 1 && !cn) continue;
    if (live_filter == 2 && cn) continue;
    qidx[qn++] = g;
  }
  int qgap = px(8);
  int qtw = (w - qgap) / 2;
  int qth = px(62);
  int qrows = screen_h > 900 ? 4 : 2;
  for (int i = 0; i < qrows * 2 && i < qn; i++) {
    int g = qidx[i];
    int x = side + (i % 2) * (qtw + qgap);
    int ty = y + (i / 2) * (qth + qgap);
    bobo_draw_rect(x, ty, qtw, qth, 1, true);
    int chip_w = px(34);
    int chip_y = ty + px(9);
    int chip_h = qth - px(18);
    bobo_fill_rect(x + px(8), chip_y, chip_w, chip_h, true);
    char num[16];
    snprintf(num, sizeof(num), "%02d", i + 1);
    bobo_draw_text(
        BOBO_FONT_SMALL,
        x + px(8) + (chip_w - text_w(BOBO_FONT_SMALL, num, true)) / 2,
        chip_y + (chip_h - slh) / 2, num, false, true);
    char shortn[48];
    group_short_name(live_group_name[g], shortn, sizeof(shortn));
    draw_ellipsized(BOBO_FONT_BODY, x + px(50), ty + px(9), qtw - px(60),
                    ui_en ? group_en_short(live_group_name[g]) : shortn, true,
                    true);
    char qsub[32];
    snprintf(qsub, sizeof(qsub), T("%d 台", "%d STATIONS"), live_group_n[g]);
    draw_ellipsized(BOBO_FONT_SMALL, x + px(50), ty + px(36), qtw - px(60),
                    qsub, true, false);
    add_hit(x, ty, qtw, qth, ACT_LIVE_GROUP, g, 0);
  }
  y += qrows * qth + (qrows - 1) * qgap + px(24);

  int fav_n = 0;
  for (int i = 0; i < live_count; i++)
    if (live_is_fav(i)) fav_n++;
  int ri[8];
  int rec_n = recent_live_indices(ri, 8);
  int small_h = px(58);
  int small_w = (w - gap) / 2;
  for (int i = 0; i < 2; i++) {
    int x = side + i * (small_w + gap);
    bobo_draw_rect(x, y, small_w, small_h, 1, true);
    draw_spaced(BOBO_FONT_SMALL, x + px(11), y + px(8),
                i == 0 ? "FAVORITES" : "RECENT", 1, true, false);
    if (i == 0)
      icon_star(x + small_w - px(11) - px(13), y + px(7), px(13), true);
    else
      icon_clock(x + small_w - px(11) - px(13), y + px(7), px(13), true);
    bobo_draw_text(BOBO_FONT_BODY, x + px(11), y + px(31),
                   i == 0 ? T("收藏", "Favorites") : T("最近播放", "Recent"),
                   true, true);
    char cnt[16];
    snprintf(cnt, sizeof(cnt), "%d", i == 0 ? fav_n : rec_n);
    draw_right(BOBO_FONT_BODY, x + small_w - px(11), y + px(31), cnt, true,
               false);
    add_hit(x, y, small_w, small_h, i == 0 ? ACT_FAVS : ACT_RECENT, 0, 0);
  }

  const char *os_lbl = T("开源项目 · ", "OPEN SOURCE · ");
  const char *os_url = "github.com/adwuard/bobo-world-radio";
  int os_lw = text_w(BOBO_FONT_SMALL, os_lbl, false);
  int os_uw = text_w(BOBO_FONT_SMALL, os_url, true);
  int os_x = (screen_w - os_lw - os_uw) / 2;
  if (os_x < px(16)) os_x = px(16);
  bobo_draw_text(BOBO_FONT_SMALL, os_x, screen_h - px(74), os_lbl, true,
                 false);
  bobo_draw_text(BOBO_FONT_SMALL, os_x + os_lw, screen_h - px(74), os_url,
                 true, true);

  draw_ruler(side, screen_h - px(46), w, 24);
  draw_center_col(BOBO_FONT_SMALL, screen_h - px(32),
                  T("点按进入 / 返回键退出", "TAP TO OPEN / BACK TO EXIT"), true,
                  false);
}

static void draw_list_page(int kind) {
  const int side = px(24);
  const int w = screen_w - 2 * side;
  const char *title =
      kind == PG_FAVS ? T("收藏", "Favorites") : T("最近播放", "Recent");
  const char *en = kind == PG_FAVS ? "FAVORITES" : "RECENTLY PLAYED";
  int y = bobo_draw_header(title) + px(16);

  int *indices = list_scratch;
  int total = 0;
  if (kind == PG_FAVS)
    total = live_fav_indices(indices, MAX_LIVE);
  else
    total = recent_live_indices(indices, MAX_LIVE);

  /* Editorial header: label, count and one rule instead of a black band. */
  if (kind == PG_FAVS)
    icon_star(side + w - px(30), y - px(4), px(30), true);
  else
    icon_clock(side + w - px(30), y - px(4), px(30), true);
  draw_spaced(BOBO_FONT_SMALL, side, y, en, px(2), true, true);
  y += px(24);
  char big[32];
  if (kind == PG_FAVS)
    snprintf(big, sizeof(big), T("%d 个收藏", "%d SAVED"), total);
  else
    snprintf(big, sizeof(big), T("%d 条记录", "%d IN HISTORY"), total);
  bobo_draw_text(BOBO_FONT_TITLE, side, y, big, true, true);
  y += bobo_line_height(BOBO_FONT_TITLE) + px(8);
  bobo_draw_text(BOBO_FONT_SMALL, side, y,
                 kind == PG_FAVS
                     ? T("本地保存 · 在电台列表点星标即可收藏",
                         "Saved on device · star a station to add it")
                     : T("按播放顺序排列 · 最多保存 8 条",
                         "Newest first · up to 8 kept"),
                 true, false);
  y += px(22);
  bobo_draw_line(side, y, side + w, y, 1, true);
  y += px(14);

  int row_h = px(64);
  int bottom = screen_h - px(54);
  int rows = (bottom - y) / row_h;
  if (rows < 3) rows = 3;
  page_rows = rows;
  page_total = total;
  if (list_first >= total) list_first = total > rows ? total - rows : 0;
  if (list_first < 0) list_first = 0;

  if (total == 0) {
    if (kind == PG_FAVS)
      draw_empty_box(y, px(84), T("还没有收藏的电台。", "No favorites yet."),
                     T("在电台列表点右侧星标即可收藏。",
                       "Tap the star on any live station row."),
                     NULL);
    else
      draw_empty_box(y, px(84), T("还没有播放记录。", "Nothing played yet."),
                     T("从直播列表选一个电台开始收听。",
                       "Pick a live station to start listening."),
                     NULL);
  } else {
    int drawn = 0;
    for (int i = list_first; i < total && drawn < rows; i++, drawn++)
      draw_live_row(&live[indices[i]], indices[i], side, y + drawn * row_h, w,
                    row_h, true);
  }
  draw_pager(ACT_PAGE, list_first, rows, total, screen_h - px(48));
}

static void group_short_name(const char *group, char *out, size_t cap) {
  const char *sep = strstr(group, "· ");
  snprintf(out, cap, "%s", sep ? sep + 3 : group);
}

static const char *group_en_short(const char *zh) {
  static char buf[48];
  const char *en = live_group_en(zh);
  const char *sep = strstr(en, " / ");
  if (sep)
    snprintf(buf, sizeof(buf), "%.*s", (int)(sep - en), en);
  else
    snprintf(buf, sizeof(buf), "%s", en);
  return buf;
}

static void draw_live_groups(void) {
  const int side = px(24);
  const int w = screen_w - 2 * side;
  int y = bobo_draw_header(T("直播电台", "Live radio")) + px(16);

  int *idx = group_scratch;
  int total = 0;
  for (int g = 0; g < live_group_count; g++) {
    bool cn = live_group_is_cn(live_group_name[g]);
    if (live_filter == 1 && !cn) continue;
    if (live_filter == 2 && cn) continue;
    idx[total++] = g;
  }

  icon_antenna(side + w - px(34), y - px(4), px(34), true);
  draw_spaced(BOBO_FONT_SMALL, side, y, "LIVE DIRECTORY", 1, true, true);
  y += px(24);
  const char *sel_zh[3] = {"全部地区", "中国", "国际"};
  const char *sel_en[3] = {"ALL REGIONS", "CHINA", "WORLD"};
  draw_spaced(BOBO_FONT_TITLE, side, y,
              ui_en ? sel_en[live_filter] : sel_zh[live_filter], px(2), true,
              true);
  y += bobo_line_height(BOBO_FONT_TITLE) + px(6);
  char sub[64];
  snprintf(sub, sizeof(sub), T("%d 台 / %d 个分组", "%d STATIONS / %d GROUPS"),
           live_group_stations(live_filter), total);
  bobo_draw_text(BOBO_FONT_SMALL, side, y, sub, true, false);
  draw_right(BOBO_FONT_SMALL, side + w, y, "AAC 64K", true, false);
  y += px(22);
  bobo_draw_line(side, y, side + w, y, 1, true);
  y += px(14);

  const char *flabels[3] = {T("全部", "ALL"), T("中国", "CHINA"),
                            T("国际", "WORLD")};
  int fcounts[3] = {live_count, live_group_stations(1), live_group_stations(2)};
  int gap = px(8);
  int bw = (w - 2 * gap) / 3;
  int bh = px(44);
  for (int i = 0; i < 3; i++) {
    int bx = side + i * (bw + gap);
    bool sel = live_filter == i;
    char label[48];
    snprintf(label, sizeof(label), "%s %d", flabels[i], fcounts[i]);
    if (sel)
      bobo_fill_round_rect(bx, y, bw, bh, px(3), true);
    else
      bobo_draw_round_rect(bx, y, bw, bh, 1, px(3), true);
    int lh = bobo_line_height(BOBO_FONT_SMALL);
    bobo_draw_text(BOBO_FONT_SMALL,
                   bx + (bw - text_w(BOBO_FONT_SMALL, label, true)) / 2,
                   y + (bh - lh) / 2, label, !sel, true);
    add_hit(bx, y, bw, bh, ACT_LIVE_FILTER, i, 0);
  }
  y += bh + px(16);

  int row_h = px(58);
  int bottom = screen_h - px(54);
  int rows = (bottom - y) / row_h;
  if (rows < 3) rows = 3;
  page_rows = rows;
  page_total = total;
  if (list_first >= total)
    list_first = total > rows ? total - rows : 0;
  if (list_first < 0) list_first = 0;

  for (int i = 0; i < rows; i++) {
    int pos = list_first + i;
    if (pos >= total) break;
    int g = idx[pos];
    int ry = y + i * row_h;
    bobo_fill_rect(side, ry + px(7), px(38), row_h - px(16), true);
    char num[16];
    snprintf(num, sizeof(num), "%02d", pos + 1);
    int nch = row_h - px(16);
    int nlh = bobo_line_height(BOBO_FONT_SMALL);
    bobo_draw_text(BOBO_FONT_SMALL, side + px(7),
                   ry + px(7) + (nch - nlh) / 2, num, false, true);
    bobo_draw_text(BOBO_FONT_BODY, side + px(50), ry + px(8),
                   ui_en ? live_group_en(live_group_name[g])
                         : live_group_name[g],
                   true, true);
    draw_spaced(BOBO_FONT_SMALL, side + px(50), ry + px(33),
                ui_en ? live_group_name[g] : live_group_en(live_group_name[g]),
                1, true, false);
    char cnt[24];
    snprintf(cnt, sizeof(cnt), T("%d 台", "%d"), live_group_n[g]);
    draw_right(BOBO_FONT_SMALL, side + w - px(30), ry + px(20), cnt, true,
               true);
    int cx = side + w - px(12);
    int cy = ry + row_h / 2;
    bobo_draw_line(cx - px(3), cy - px(4), cx + px(1), cy, 1, true);
    bobo_draw_line(cx + px(1), cy, cx - px(3), cy + px(4), 1, true);
    row_basics(side, ry, w, row_h, ACT_LIVE_GROUP, g);
  }
  draw_pager(ACT_PAGE, list_first, rows, total, screen_h - px(48));
}

static void draw_live_stations(int g) {
  if (g < -1 || g >= live_group_count) g = 0;
  const int side = px(24);
  const int w = screen_w - 2 * side;
  const int first = g < 0 ? 0 : live_group_first[g];
  const int total = g < 0 ? live_group_stations(1) : live_group_n[g];
  const char *nzh = g < 0 ? "中国" : live_group_name[g];
  const char *nen = g < 0 ? "CHINA" : live_group_en(live_group_name[g]);
  int y = bobo_draw_header(T("直播电台", "Live radio")) + px(16);

  icon_antenna(side + w - px(34), y - px(4), px(34), true);
  draw_spaced(BOBO_FONT_SMALL, side, y, "LIVE RADIO", 1, true, true);
  y += px(24);
  draw_ellipsized(BOBO_FONT_TITLE, side, y, w - px(60), ui_en ? nen : nzh,
                  true, true);
  y += bobo_line_height(BOBO_FONT_TITLE) + px(6);
  draw_spaced(BOBO_FONT_SMALL, side, y, ui_en ? nzh : nen, 1, true, false);
  char sub[48];
  snprintf(sub, sizeof(sub), T("%d 台 · AAC 直播", "%d · AAC LIVE"), total);
  draw_right(BOBO_FONT_SMALL, side + w, y, sub, true, false);
  y += px(22);
  bobo_draw_line(side, y, side + w, y, 1, true);
  y += px(14);

  int row_h = px(64);
  int bottom = screen_h - px(54);
  int rows = (bottom - y) / row_h;
  if (rows < 3) rows = 3;
  page_rows = rows;
  page_total = total;
  if (list_first >= total) list_first = total > rows ? total - rows : 0;
  if (list_first < 0) list_first = 0;

  int drawn = 0;
  for (int i = list_first; i < total && drawn < rows; i++, drawn++)
    draw_live_row(&live[first + i], first + i, side, y + drawn * row_h, w,
                  row_h, true);
  draw_pager(ACT_PAGE, list_first, rows, total, screen_h - px(48));
}

static void draw_about(void) {
  const int side = px(24);
  const int w = screen_w - 2 * side;
  const int slh = bobo_line_height(BOBO_FONT_SMALL);
  const int tlh = bobo_line_height(BOBO_FONT_TITLE);
  int y = bobo_draw_header(T("关于", "About")) + px(16);

  icon_globe(side + w - px(46), y - px(3), px(46), true);
  draw_spaced(BOBO_FONT_SMALL, side, y, "ABOUT THIS APP", px(2), true, true);
  y += px(24);
  if (ui_en)
    draw_spaced(BOBO_FONT_TITLE, side, y, "WORLD RADIO", px(3), true, true);
  else
    bobo_draw_text(BOBO_FONT_TITLE, side, y, "环球电台", true, true);
  y += tlh + px(8);
  char ver[64];
  snprintf(ver, sizeof(ver), "VERSION %s · API 6 · AAC/HLS", APP_VERSION);
  bobo_draw_text(BOBO_FONT_SMALL, side, y, ver, true, false);
  y += px(22);
  bobo_draw_line(side, y, side + w, y, 1, true);
  y += px(18);

  int nlh = slh + px(4);
  char *desc = about_desc_buf;
  snprintf(desc, sizeof(about_desc_buf),
           T("墨水屏上的世界收音机：%d 家直播电台、%d 个地区分组，"
             "HLS 分片续播，无点播、无广告。",
             "A world radio for e-ink. %d live internet stations in %d "
             "regions, streamed as HLS segments. No on-demand or ads."),
           live_count, live_group_count);
  y += draw_wrap(BOBO_FONT_SMALL, desc, side, y, w, 4, nlh, false, true) * nlh;
  y += px(26);

  draw_spaced(BOBO_FONT_SMALL, side, y, "LIVE CATALOG", px(2), true, true);
  draw_right(BOBO_FONT_SMALL, side + w, y, "HLS / AAC-LC", true, false);
  y += px(24);
  int cw = w / 4;
  char v0[16], v1[16];
  snprintf(v0, sizeof(v0), "%d", live_count);
  snprintf(v1, sizeof(v1), "%d", live_group_count);
  const char *vals[4] = {v0, v1, "64k", "7s"};
  const char *labs[4] = {T("电台", "STATIONS"), T("分组", "REGIONS"), "AAC-LC",
                         T("分片", "HLS SEG")};
  for (int i = 0; i < 4; i++) {
    int cx = side + i * cw + cw / 2;
    bobo_draw_text(BOBO_FONT_TITLE,
                   cx - text_w(BOBO_FONT_TITLE, vals[i], true) / 2, y, vals[i],
                   true, true);
    bobo_draw_text(BOBO_FONT_SMALL,
                   cx - text_w(BOBO_FONT_SMALL, labs[i], false) / 2,
                   y + px(28), labs[i], true, false);
    if (i > 0)
      bobo_draw_line(side + i * cw, y - px(2), side + i * cw, y + px(46), 1,
                     true);
  }
  y += px(54);
  bobo_draw_line(side, y, side + w, y, 1, true);
  y += px(24);

  icon_gh(side, y - px(2), px(40), true);
  draw_spaced(BOBO_FONT_SMALL, side + px(56), y, "BUILD", px(2), true, true);
  bobo_draw_text(BOBO_FONT_BODY, side + px(56), y + px(17),
                 "Built by @adwuard", true, true);
  bobo_draw_text(BOBO_FONT_SMALL, side + px(56), y + px(41),
                 "github.com/adwuard", true, false);
  y += px(56);
  bobo_draw_line(side, y, side + w, y, 1, true);
  y += px(24);

  draw_spaced(BOBO_FONT_SMALL, side, y, "LANGUAGE", px(2), true, true);
  draw_right(BOBO_FONT_SMALL, side + w, y, T("语言", "LANGUAGE"), true, false);
  y += px(24);

  int gap = px(8);
  int bw = (w - 2 * gap) / 3;
  int bh = px(48);
  const char *labels[3] = {T("跟随系统", "SYSTEM"), "中文", "English"};
  int modes[3] = {0, 1, 2};
  int blh = bobo_line_height(BOBO_FONT_SMALL);
  for (int i = 0; i < 3; i++) {
    int bx = side + i * (bw + gap);
    bool sel = lang_mode == modes[i];
    if (sel)
      bobo_fill_round_rect(bx, y, bw, bh, px(3), true);
    else
      bobo_draw_round_rect(bx, y, bw, bh, 1, px(3), true);
    bobo_draw_text(BOBO_FONT_SMALL,
                   bx + (bw - text_w(BOBO_FONT_SMALL, labels[i], true)) / 2,
                   y + (bh - blh) / 2, labels[i], !sel, true);
    add_hit(bx, y, bw, bh, ACT_LANG_AUTO + i, 0, 0);
  }
  y += bh + px(10);

  if (lang_mode == 0)
    y += draw_wrap(BOBO_FONT_SMALL,
                   T("自动模式会跟随系统语言。当前 SDK 未提供系统语言接口，"
                     "暂时以中文显示；可在上表选择。",
                     "Auto follows the system language. API 6 exposes no "
                     "locale call yet, so Chinese is used for now; pick a "
                     "language above to override."),
                   side, y, w, 2, nlh, false, true) *
             nlh +
         px(14);
  else
    y += px(14);

  draw_spaced(BOBO_FONT_SMALL, side, y, "PLAYER CONTROLS", px(2), true, true);
  y += px(24);
  bobo_draw_text(BOBO_FONT_SMALL, side, y,
                 T("播放 / 停止 一键启停直播 · 上一台 / 下一台 点文字条换台",
                   "PLAY / STOP one key toggles · PREV / NEXT tap the names"),
                 true, false);
  bobo_draw_text(BOBO_FONT_SMALL, side, y + nlh,
                 T("音量 跟随系统设置（侧键）· 返回 回到电台列表",
                   "VOLUME follows the system · BACK returns to the list"),
                 true, false);
  y += nlh * 2 + px(14);

  draw_spaced(BOBO_FONT_SMALL, side, y, "CREDITS", px(2), true, true);
  y += px(22);
  draw_wrap(BOBO_FONT_SMALL,
            T("Helix AAC-LC (RPSL) · 直播目录来自各广播机构",
              "Helix AAC-LC (RPSL) · live catalogs from public broadcasters"),
            side, y, w, 2, nlh, false, true);

  draw_center_col(BOBO_FONT_SMALL, screen_h - px(28),
                  T("返回键回到上一页", "Back returns to the previous page"),
                  true, false);
}

static void draw_data_error(void) {
  int y = bobo_draw_header(T("环球电台", "World Radio")) + px(24);
  const int side = px(24);
  const int w = screen_w - 2 * side;
  bobo_fill_rect(side, y, w, px(72), true);
  bobo_draw_text(BOBO_FONT_TITLE, side + px(14), y + px(14),
                 T("目录数据缺失", "CATALOG MISSING"), false, true);
  bobo_draw_text(BOBO_FONT_SMALL, side + px(14), y + px(46), "FILES / NOT FOUND",
                 false, false);
  y += px(72) + px(20);
  if (ui_en) {
    bobo_draw_text(BOBO_FONT_BODY, side, y, "live.tsv", true, true);
    bobo_draw_text(BOBO_FONT_BODY, side, y + px(26), "was not installed.", true,
                   true);
  } else {
    bobo_draw_text(BOBO_FONT_BODY, side, y, "应用包里的 live.tsv 没有安装。",
                   true, true);
    bobo_draw_text(BOBO_FONT_BODY, side, y + px(26), "直播目录为空。", true,
                   true);
  }
  y += px(70);
  bobo_draw_text(BOBO_FONT_SMALL, side, y,
                 T("请重新安装应用，并确认 files/ 随包分发。",
                   "Please reinstall the app and keep files/ in the package."),
                 true, false);
  y += px(20);
  bobo_draw_text(BOBO_FONT_SMALL, side, y, T("返回键退出。", "Back exits."),
                 true, false);
}

static void draw_page(int refresh) {
  hit_count = 0;
  bobo_clear();
  if (!live_ok) {
    draw_data_error();
    present(refresh);
    return;
  }
  switch (page) {
    case PG_HOME:
      draw_home();
      break;
    case PG_FAVS:
    case PG_RECENT:
      draw_list_page(page);
      break;
    case PG_LIVE:
      draw_live_groups();
      break;
    case PG_LIVE_ST:
      draw_live_stations(page_arg);
      break;
    case PG_ABOUT:
      draw_about();
      break;
    default:
      draw_home();
      break;
  }
  present(refresh);
}

/* ---- live player -------------------------------------------------------- */

#define LIVE_NET_CAP (448 * 1024)

static void live_draw(int refresh);
static void live_tap(int x, int y);
static void live_close_audio(void);
static bool live_pump(void);

static void live_cleanup(void) {
  live_close_audio();
  if (live_dec) {
    AACFreeDecoder(live_dec);
    live_dec = NULL;
  }
  free(live_net);
  live_net = NULL;
  free(live_ts);
  live_ts = NULL;
  if (bobo_capabilities() & BOBO_CAP_KEEP_AWAKE) bobo_keep_awake(false);
}

static int live_fetch(const char *url, size_t *len, uint32_t *ms) {
  static const bobo_header_t hdr[] = {
      {"User-Agent", "WorldRadio/0.3"},
      {"Accept", "*/*"},
      {"Accept-Encoding", "identity"},
  };
  size_t n = 0;
  *len = 0;
  *ms = 0;
  if (!live_pump()) return -1;
  if (live_quit || live_exit || live_stop_req) return -1;
  uint32_t t0 = bobo_millis();
  int st = bobo_http("GET", url, hdr, 3, NULL, 0, (char *)live_net,
                     LIVE_NET_CAP, &n);
  *ms = bobo_millis() - t0;
  *len = n;
  live_pump();
  bobo_log("live: fetch %s status=%d bytes=%u ms=%u", url, st, (unsigned)n,
           (unsigned)*ms);
  return st;
}

static void live_parse_playlist(size_t len) {
  int total = 0;
  size_t i = 0;
  while (i < len) {
    size_t j = i;
    while (j < len && live_net[j] != '\n') j++;
    size_t e = j;
    if (e > i && live_net[e - 1] == '\r') e--;
    const char *line = (const char *)live_net + i;
    size_t n = e - i;
    if (n > 0 && line[0] != '#' && n < sizeof(live_seg[0])) {
      memcpy(live_seg[total % MAX_LIVE_SEG], line, n);
      live_seg[total % MAX_LIVE_SEG][n] = '\0';
      total++;
    }
    i = j + 1;
  }
  live_seg_count = total < MAX_LIVE_SEG ? total : MAX_LIVE_SEG;
  if (total > MAX_LIVE_SEG) {
    int shift = total % MAX_LIVE_SEG;
    for (int r = 0; r < shift; r++) {
      memcpy(live_seg_tmp, live_seg[0], sizeof(live_seg_tmp));
      memmove(live_seg[0], live_seg[1],
              sizeof(live_seg[0]) * (MAX_LIVE_SEG - 1));
      memcpy(live_seg[MAX_LIVE_SEG - 1], live_seg_tmp, sizeof(live_seg_tmp));
    }
  }
  bobo_log("live: playlist lines=%d keep=%d", total, live_seg_count);
}

static void url_set_base(const char *url) {
  snprintf(url_tmp, sizeof(url_tmp), "%s", url);
  char *q = strchr(url_tmp, '?');
  if (q) *q = '\0';
  char *scheme = strstr(url_tmp, "://");
  char *slash = scheme ? strrchr(scheme + 3, '/') : NULL;
  if (slash) {
    slash[1] = '\0';
    snprintf(live_base, sizeof(live_base), "%s", url_tmp);
  } else if (scheme) {
    snprintf(live_base, sizeof(live_base), "%s/", url_tmp);
  } else {
    live_base[0] = '\0';
  }
}

static void live_make_url(const char *seg) {
  if (strncmp(seg, "https://", 8) == 0 || strncmp(seg, "http://", 7) == 0)
    snprintf(live_url, sizeof(live_url), "%s", seg);
  else if (strncmp(seg, "//", 2) == 0)
    snprintf(live_url, sizeof(live_url), "https:%s", seg);
  else if (seg[0] == '/') {
    const char *scheme = strstr(live_base, "://");
    const char *host = scheme ? scheme + 3 : live_base;
    const char *slash = strchr(host, '/');
    int n = slash ? (int)(slash - live_base) : (int)strlen(live_base);
    snprintf(live_url, sizeof(live_url), "%.*s%s", n, live_base, seg);
  } else {
    int room = (int)sizeof(live_url) - (int)strlen(seg) - 1;
    int blen = (int)strlen(live_base);
    if (room > blen) room = blen;
    if (room < 0) room = 0;
    snprintf(live_url, sizeof(live_url), "%.*s%s", room, live_base, seg);
  }
}

static bool live_resolve_master(char *pl, size_t cap, size_t len) {
  long best_bw = -1;
  size_t best_off = 0, best_len = 0;
  long bw = -1;
  size_t i = 0;
  while (i < len) {
    size_t j = i;
    while (j < len && live_net[j] != '\n') j++;
    size_t e = j;
    if (e > i && live_net[e - 1] == '\r') e--;
    size_t n = e - i;
    if (n > 0 && n < sizeof(pl_line)) {
      memcpy(pl_line, live_net + i, n);
      pl_line[n] = '\0';
      if (pl_line[0] == '#') {
        if (!strncmp(pl_line, "#EXT-X-STREAM-INF", 17)) {
          const char *b = strstr(pl_line, "BANDWIDTH=");
          bw = b ? strtol(b + 10, NULL, 10) : 0;
        }
      } else {
        if (bw >= 0 && (best_bw < 0 || bw < best_bw)) {
          best_bw = bw;
          best_off = i;
          best_len = n;
        }
        bw = -1;
      }
    }
    i = j + 1;
  }
  if (best_bw < 0 || best_len >= sizeof(pl_variant)) return false;
  memcpy(pl_variant, live_net + best_off, best_len);
  pl_variant[best_len] = '\0';
  live_make_url(pl_variant);
  snprintf(pl, cap, "%s", live_url);
  url_set_base(pl);
  bobo_log("live: master bw=%ld -> %s", best_bw, pl);
  return true;
}

static size_t live_demux_ts(const uint8_t *src, size_t len) {
  size_t pkt = 188;
  if (len >= 388 && src[0] != 0x47 && src[4] == 0x47) {
    src += 4;
    len -= 4;
    pkt = 192;
  } else if (len >= 408 && src[0] == 0x47 && src[204] == 0x47) {
    pkt = 204;
  }
  int audio_pid = -1;
  size_t out = 0;
  for (size_t off = 0; off + pkt <= len; off += pkt) {
    const uint8_t *p = src + off;
    if (p[0] != 0x47) continue;
    int pid = ((p[1] & 0x1f) << 8) | p[2];
    bool pusi = (p[1] & 0x40) != 0;
    int afc = (p[3] >> 4) & 3;
    size_t i = 4;
    if (afc & 2) {
      if (i >= pkt) continue;
      i += 1 + p[4];
    }
    if (!(afc & 1) || i >= pkt) continue;
    if (pusi && i + 9 <= pkt && p[i] == 0 && p[i + 1] == 0 && p[i + 2] == 1) {
      int sid = p[i + 3];
      if (sid >= 0xC0 && sid <= 0xDF) {
        if (audio_pid < 0) audio_pid = pid;
        i += 9 + p[i + 8];
      }
    }
    if (audio_pid < 0 || pid != audio_pid || i > pkt) continue;
    size_t n = pkt - i;
    if (out + n > LIVE_NET_CAP) break;
    memcpy(live_ts + out, p + i, n);
    out += n;
  }
  bobo_log("live: ts pid=%d payload=%u", audio_pid, (unsigned)out);
  return out;
}

static int live_find(const char *seg) {
  for (int i = 0; i < live_seg_count; i++)
    if (!strcmp(live_seg[i], seg)) return i;
  return -1;
}

static const char *live_state_label(void) {
  switch (live_state) {
    case LV_CONNECTING:
      return T("连接中", "CONNECTING");
    case LV_PLAYING:
      return T("直播中", "ON AIR");
    case LV_BUFFERING:
      return T("缓冲中", "BUFFERING");
    case LV_STOPPED:
      return T("已停止", "STOPPED");
    case LV_INTERRUPTED:
      return T("已中断", "INTERRUPTED");
    default:
      return T("无法收听", "OFFLINE");
  }
}

static bool live_on_air(void) {
  return live_state == LV_PLAYING || live_state == LV_BUFFERING ||
         live_state == LV_INTERRUPTED || live_state == LV_CONNECTING;
}

static void live_close_audio(void) {
  if (live_audio) {
    bobo_audio_close();
    live_audio = false;
  }
}

static bool live_handle_event(const bobo_event_t *ev) {
  switch (ev->type) {
    case BOBO_EV_EXIT:
      live_exit = true;
      return false;
    case BOBO_EV_REDRAW:
      live_draw(BOBO_REFRESH_HALF);
      break;
    case BOBO_EV_BUTTON:
      if (ev->button == BOBO_BTN_BACK) live_quit = true;
      break;
    case BOBO_EV_TAP:
      live_tap(ev->x, ev->y);
      break;
    case BOBO_EV_AUDIO:
      if (ev->button == BOBO_AUDIO_EV_INTERRUPTED && live_on_air()) {
        live_state = LV_INTERRUPTED;
        live_intr_t0 = bobo_millis();
        strcpy(live_note, T("语音助手正在使用扬声器，稍后自动继续。",
                            "The assistant has the speaker; listening resumes "
                            "when it is free."));
        live_draw(BOBO_REFRESH_FAST);
      } else if (ev->button == BOBO_AUDIO_EV_AVAILABLE &&
                 live_state == LV_INTERRUPTED) {
        if (bobo_audio_pause(false) == BOBO_AUDIO_OK) {
          live_state = LV_PLAYING;
          live_note[0] = '\0';
          live_route = -1;
          live_wd_armed = false;
        }
        live_draw(BOBO_REFRESH_FAST);
      }
      break;
    default:
      break;
  }
  return true;
}

/* Handles everything already queued without waiting. Called right after a
 * blocking fetch and between decoded frames so BACK and STOP react within a
 * frame even mid-segment. */
static bool live_pump(void) {
  bobo_event_t ev;
  for (int n = 0; n < 64 && bobo_wait_event(&ev, 0); n++)
    if (!live_handle_event(&ev)) return false;
  return true;
}

static bool live_poll(void) {
  bobo_event_t ev;
  if (!bobo_wait_event(&ev, 20)) return true;
  return live_handle_event(&ev);
}

/* Drop input still queued when the player closes: a held or repeated BACK
 * must not navigate the page behind, and a stray tap must not reopen a
 * station. */
static void live_drain_input(void) {
  bobo_event_t ev;
  for (int n = 0; n < 128 && bobo_wait_event(&ev, 0); n++)
    if (ev.type == BOBO_EV_EXIT) live_exit = true;
}

static void live_wait(uint32_t ms) {
  uint32_t end = bobo_millis() + ms;
  while (!live_quit && !live_exit && !live_stop_req && !live_tune &&
         (int32_t)(end - bobo_millis()) > 0)
    live_poll();
}

/* Drops the old sink's queue and lets the next decoded frame reopen the
 * output. A Bluetooth connect or disconnect reroutes the host stream and can
 * leave it silent, so the queue, the format and the drop count start over. */
static void live_restart_audio(const char *zh, const char *en) {
  live_close_audio();
  live_hz = live_ch = 0;
  live_priming = false;
  live_prime_resume = false;
  live_last_prime = 0;
  live_route = -1;
  live_wd_armed = false;
  live_underruns = 0;
  live_state = LV_CONNECTING;
  strcpy(live_note, T(zh, en));
  live_last_ui = 0;
  live_draw(BOBO_REFRESH_FAST);
}

/* The SDK sends no route event, so the sink is polled (4 Hz) while audio is
 * open. Moving to or from a Bluetooth headset reroutes the host stream; the
 * new sink also takes a moment to come up, so the change is debounced and
 * then the output is reopened and primed from scratch. */
static void live_watch_route(void) {
  if (!live_audio || !live_on_air() || live_state == LV_INTERRUPTED) return;
  uint32_t now = bobo_millis();
  if (live_route >= 0 && (uint32_t)(now - live_route_ms) < 250) return;
  live_route_ms = now;
  char rn[40];
  int r = bobo_audio_route(rn, sizeof(rn));
  if (r != BOBO_AUDIO_ROUTE_SPEAKER && r != BOBO_AUDIO_ROUTE_HEADSET) return;
  if (live_route < 0) {
    live_route = r;
    return;
  }
  if (r == live_route) return;

  /* A2DP setup can make the route flap; wait until it holds still. */
  int stable = 0;
  for (int i = 0; i < 15; i++) {
    if (live_quit || live_exit || live_stop_req || live_tune) return;
    live_wait(100);
    char nm[40];
    int rr = bobo_audio_route(nm, sizeof(nm));
    if (rr == r) {
      if (++stable >= 3) break;
    } else {
      r = rr;
      stable = 0;
    }
  }
  if (live_quit || live_exit || live_stop_req || live_tune) return;
  if (r == live_route ||
      (r != BOBO_AUDIO_ROUTE_SPEAKER && r != BOBO_AUDIO_ROUTE_HEADSET))
    return;
  bobo_log("live: route %d -> %d", live_route, r);
  live_route = r;
  live_restart_audio("音频输出已切换，正在重新缓冲…",
                     "Audio output changed, rebuffering...");
}

/* A rerouted sink can keep taking audio that is never heard; every other
 * counter looks healthy. If the played position stands still for 4 s with
 * audio still queued, reopen the output once. */
static void live_watch_played(void) {
  uint32_t now = bobo_millis();
  if (!live_wd_armed) {
    live_wd_armed = true;
    live_wd_t0 = now;
    live_wd_played = bobo_audio_played();
    return;
  }
  if ((uint32_t)(now - live_wd_t0) < 4000) return;
  uint32_t played = bobo_audio_played();
  if (played != live_wd_played || bobo_audio_buffered_ms() == 0) {
    live_wd_t0 = now;
    live_wd_played = played;
    return;
  }
  bobo_log("live: played stuck at %u, reopening output", (unsigned)played);
  live_restart_audio("音频输出没有出声，正在重启…",
                     "The output went silent; restarting audio...");
}

/* Holds the output while the host ring fills. A fresh open and a starved
 * buffer are the two moments that break up on a Bluetooth headset; the feed
 * starts again only once a few seconds are queued. */
static void live_start_prime(uint32_t target_ms, bool resume) {
  if (target_ms == 0) {
    char rn[40];
    int route = bobo_audio_route(rn, sizeof(rn));
    target_ms = route == BOBO_AUDIO_ROUTE_HEADSET ? 3500 : 2000;
  }
  live_prime_ms = target_ms;
  live_prime_t0 = bobo_millis();
  live_priming = true;
  live_prime_resume = resume;
  bobo_audio_pause(true);
}

/* Pulls the playlist forward while the ring is full, so the fetch at the end
 * of the window never runs on an empty buffer. True when a segment after
 * live_cur is listed. */
static bool live_refresh_next(void) {
  for (int tries = 0;
       tries < 3 && !live_quit && !live_exit && !live_stop_req && !live_tune;
       tries++) {
    size_t pl_len = 0;
    uint32_t pl_ms = 0;
    int pst = live_fetch(live_pl_url, &pl_len, &pl_ms);
    live_fetch_ms = pl_ms;
    if (pst == 200 && pl_len > 0) {
      live_parse_playlist(pl_len);
      int pos = live_find(live_cur);
      if (pos >= 0 && pos + 1 < live_seg_count) return true;
    }
    if (tries < 2) live_wait(500);
  }
  return false;
}

static bool live_write_paced(const int16_t *buf, size_t frames, int ch) {
  size_t off = 0;
  while (off < frames) {
    if (live_quit || live_exit || live_stop_req || live_tune) return false;
    live_watch_route();
    if (live_quit || live_exit || live_stop_req || live_tune) return false;
    if (!live_audio) return true; /* restarted; the next frame reopens */
    if (live_state == LV_INTERRUPTED) {
      if (!live_pump()) return false;
      if (live_quit || live_exit || live_stop_req || live_tune) return false;
      /* An interruption that never reports available would hold the stream
         forever; try the speaker again every 6 s. */
      if ((uint32_t)(bobo_millis() - live_intr_t0) >= 6000) {
        live_intr_t0 = bobo_millis();
        if (bobo_audio_pause(false) == BOBO_AUDIO_OK) {
          live_state = LV_PLAYING;
          live_note[0] = '\0';
          live_route = -1;
          live_wd_armed = false;
          live_draw(BOBO_REFRESH_FAST);
        }
      }
      if (live_last_ui == 0 || bobo_millis() - live_last_ui >= 30000)
        live_draw(BOBO_REFRESH_FAST);
      bobo_delay(20);
      continue;
    }
    if (live_priming) {
      uint32_t elapsed = bobo_millis() - live_prime_t0;
      uint32_t buffered = bobo_audio_buffered_ms();
      /* A paused output that accepts no frames must not keep us silent. */
      if (buffered >= live_prime_ms || elapsed >= 2000 ||
          (buffered == 0 && elapsed >= 400)) {
        live_priming = false;
        bobo_audio_pause(false);
        if (live_prime_resume) live_state = LV_PLAYING;
        live_prime_resume = false;
        live_last_ui = 0;
      }
    }
    size_t k = bobo_audio_write(buf + off * (size_t)ch, frames - off);
    if (!k) {
      if (!live_pump()) return false;
      if (live_quit || live_exit || live_stop_req || live_tune) return false;
      /* A full ring whose position no longer moves is a dead sink too. */
      if (!live_priming && live_state == LV_PLAYING) live_watch_played();
      if (!live_audio) return true; /* restarted; the next frame reopens */
      if (!live_priming && live_state == LV_PLAYING &&
          bobo_audio_buffered_ms() == 0 &&
          (uint32_t)(bobo_millis() - live_last_prime) >= 2000) {
        live_last_prime = bobo_millis();
        live_underruns++;
        live_state = LV_BUFFERING;
        live_start_prime(1500, true);
        live_draw(BOBO_REFRESH_FAST);
        continue;
      }
      bobo_delay(20);
      if (live_last_ui == 0 || bobo_millis() - live_last_ui >= 30000)
        live_draw(BOBO_REFRESH_FAST);
    } else {
      off += k;
      if (!live_priming && live_state == LV_PLAYING) live_watch_played();
    }
  }
  return true;
}

static bool live_open_audio(int hz, int ch) {
  if (live_audio && hz == live_hz && ch == live_ch) return true;
  if (hz < 8000 || hz > 192000 || ch < 1 || ch > 8) {
    snprintf(live_note, sizeof(live_note),
             T("AAC 参数不支持：%d Hz / %d 声道。",
               "Unsupported AAC: %d Hz / %d ch."),
             hz, ch);
    return false;
  }
  bool rate_change = !live_audio || hz != live_hz;
  bool was_playing = live_state == LV_PLAYING;
  int rc = bobo_audio_open((uint32_t)hz, ch);
  /* A sink that just appeared needs a moment; retry before the segment is
     written off as failed. */
  for (int i = 0; rc != BOBO_AUDIO_OK && i < 3; i++) {
    bobo_log("live: audio open retry rc=%d", rc);
    live_wait(400);
    if (live_quit || live_exit || live_stop_req || live_tune) return false;
    rc = bobo_audio_open((uint32_t)hz, ch);
  }
  bobo_log("live: audio open rc=%d hz=%d ch=%d", rc, hz, ch);
  if (rc != BOBO_AUDIO_OK) {
    snprintf(live_note, sizeof(live_note),
             T("扬声器打开失败（%d）。", "Speaker open failed (%d)."), rc);
    return false;
  }
  live_audio = true;
  live_hz = hz;
  live_ch = ch;
  live_route = -1; /* adopt whichever sink this open landed on */
  live_route_ms = bobo_millis();
  live_wd_armed = false;
  if (bobo_capabilities() & BOBO_CAP_KEEP_AWAKE) bobo_keep_awake(true);
  /* A channel-count change keeps the queued audio, so it needs no refill;
     a sample-rate change drops it and must be primed again. */
  if (rate_change) {
    live_start_prime(0, was_playing);
    if (was_playing) live_state = LV_BUFFERING;
  }
  return true;
}

static bool live_play_segment(const char *seg) {
  if (!live_pump()) return false;
  if (live_quit || live_exit || live_stop_req || live_tune) return false;
  live_make_url(seg);
  size_t len = 0;
  uint32_t ms = 0;
  int st = live_fetch(live_url, &len, &ms);
  live_fetch_ms = ms;
  if (live_quit || live_exit || live_stop_req || live_tune) return false;
  if (st != 200 || len < 8) {
    snprintf(live_note, sizeof(live_note),
             T("分片下载失败：HTTP %d。", "Segment download failed: HTTP %d."),
             st);
    return false;
  }

  const uint8_t *data = live_net;
  size_t dlen = len;
  if (live_net[0] == 0x47 || (len >= 380 && live_net[4] == 0x47)) {
    dlen = live_demux_ts(live_net, len);
    if (dlen < 8) {
      strcpy(live_note,
             T("TS 分片里没有音频数据。", "No audio data in this TS segment."));
      return false;
    }
    data = live_ts;
  } else if (len >= 12 && !memcmp(live_net + 4, "ftyp", 4)) {
    strcpy(live_note, T("这个电台使用 fMP4 直播，暂不支持。",
                        "This station streams fMP4; not supported yet."));
    return false;
  }

  unsigned char *p = (unsigned char *)data;
  int left = (int)dlen;
  int got = 0;
  uint32_t dec_ms = 0;
  while (left > 7) {
    if (!live_pump()) return false;
    if (live_quit || live_exit || live_stop_req || live_tune) return false;
    live_watch_route();
    if (live_quit || live_exit || live_stop_req || live_tune) return false;
    uint32_t t0 = bobo_millis();
    int err = AACDecode(live_dec, &p, &left, live_pcm);
    dec_ms += bobo_millis() - t0;
    if (err) {
      if (err != ERR_AAC_INDATA_UNDERFLOW)
        bobo_log("live: decode err=%d left=%d", err, left);
      break;
    }
    AACFrameInfo fi;
    AACGetLastFrameInfo(live_dec, &fi);
    int ch = fi.nChans > 0 ? fi.nChans : 1;
    if (!live_open_audio(fi.sampRateOut, ch)) return false;
    size_t n = (size_t)fi.outputSamps / (size_t)ch;
    if (n * (size_t)ch > sizeof(live_pcm) / sizeof(live_pcm[0]))
      n = sizeof(live_pcm) / sizeof(live_pcm[0]) / (size_t)ch;
    if (n) {
      if (!live_write_paced(live_pcm, n, ch)) return false;
      live_frames += (uint32_t)n;
    }
    got++;
    if (live_state == LV_CONNECTING && !live_priming) {
      live_state = LV_PLAYING;
      live_note[0] = '\0';
      live_last_ui = 0;
    }
  }
  live_decode_ms = dec_ms;
  if (!got) {
    strcpy(live_note, T("分片里没有可解码的 AAC 帧。",
                          "Segment has no decodable AAC frames."));
    return false;
  }
  return true;
}

static void live_channel_of(int idx, int *channel, int *total) {
  *channel = 1;
  *total = 1;
  if (idx < 0) return;
  for (int g = 0; g < live_group_count; g++) {
    int first = live_group_first[g];
    if (idx >= first && idx < first + live_group_n[g]) {
      *channel = idx - first + 1;
      *total = live_group_n[g];
      return;
    }
  }
}

static void stat_labelled(int x, int y, const char *label, const char *value) {
  bobo_draw_text(BOBO_FONT_SMALL, x, y, label, true, false);
  bobo_draw_text(BOBO_FONT_SMALL,
                 x + text_w(BOBO_FONT_SMALL, label, false) + px(7), y, value,
                 true, true);
}

/* One primary key that starts and stops the stream; its icon and label follow
 * the state. Switching stations stays on the tune strip below. */
static void draw_transport(int x, int y, int w, int h) {
  bool playing = live_on_air();
  const char *label = playing ? T("停止", "STOP") : T("播放", "PLAY");
  int ic = px(14);
  int lw = text_w(BOBO_FONT_BODY, label, true);
  int gx = x + (w - (ic + px(9) + lw)) / 2;
  int mid = y + h / 2;
  if (playing)
    icon_stop(gx, mid - ic / 2, ic, true);
  else
    icon_play(gx, mid - ic / 2, ic, true);
  bobo_draw_text(BOBO_FONT_BODY, gx + ic + px(9),
                 mid - bobo_line_height(BOBO_FONT_BODY) / 2 + px(1), label,
                 true, true);
  add_hit(x, y, w, h, ACT_LIVE_PLAY, 0, 0);
  bobo_draw_rect(x, y, w, h, 2, true);
}

/* Which stations the prev/next keys will reach, shown on either side so tuning
 * is not a blind jump. Both halves are tap targets too. */
static void draw_tune_strip(int x, int y, int w, int h) {
  int half = w / 2;
  int g = live_group_of(live_idx);
  int first = live_group_first[g];
  int n = live_group_n[g];
  int prev = live_idx, next = live_idx;
  if (live_idx >= 0 && n > 1) {
    int pos = live_idx - first;
    prev = first + (pos - 1 + n) % n;
    next = first + (pos + 1) % n;
  }
  bobo_draw_rect(x, y, w, h, 1, true);
  bobo_draw_line(x + half, y + 1, x + half, y + h - 2, 1, true);
  int lh = bobo_line_height(BOBO_FONT_BODY);
  int slh = bobo_line_height(BOBO_FONT_SMALL);
  int ty = y + (h - lh) / 2;
  int ic = px(12);
  int iy = y + (h - ic) / 2;
  const char *cap_prev = T("上一台", "PREV");
  int capw = text_w(BOBO_FONT_SMALL, cap_prev, false);
  icon_skip(x + px(10), iy, ic, true, false);
  bobo_draw_text(BOBO_FONT_SMALL, x + px(10) + ic + px(5), y + (h - slh) / 2,
                 cap_prev, true, false);
  int name_x = x + px(10) + ic + px(5) + capw + px(8);
  draw_ellipsized(BOBO_FONT_BODY, name_x, ty, x + half - px(8) - name_x,
                  prev >= 0 ? live[prev].name : "", true, true);
  const char *cap_next = T("下一台", "NEXT");
  int capn = text_w(BOBO_FONT_SMALL, cap_next, false);
  int right_block = ic + px(5) + capn + px(10);
  draw_ellipsized(BOBO_FONT_BODY, x + half + px(10), ty,
                  w - half - right_block - px(18),
                  next >= 0 ? live[next].name : "", true, true);
  bobo_draw_text(BOBO_FONT_SMALL, x + w - px(10) - ic - px(5) - capn,
                 y + (h - slh) / 2, cap_next, true, false);
  icon_skip(x + w - px(10) - ic, iy, ic, true, true);
  add_hit(x, y, half, h, ACT_LIVE_PREV, 0, 0);
  add_hit(x + half, y, w - half, h, ACT_LIVE_NEXT, 0, 0);
}

static void live_draw(int refresh) {
  hit_count = 0;
  bobo_clear();
  const int side = px(24);
  const int w = screen_w - 2 * side;
  const char *name =
      live_idx >= 0 ? live[live_idx].name : T("直播电台", "LIVE RADIO");
  const char *sub = live_idx >= 0 ? live[live_idx].sub : "";
  const char *group = live_idx >= 0 ? live[live_idx].group : "";
  int channel = 1, total = 1;
  live_channel_of(live_idx, &channel, &total);
  bool aired = live_on_air();
  int y = bobo_draw_header(T("直播电台", "Live radio")) + px(8);

  /* Sized to the drawing so no empty band sits above the radio. */
  int art_bh = screen_h > 900 ? px(152) : px(120);
  int card = px(76) + art_bh + px(40);
  draw_spaced(BOBO_FONT_SMALL, side + px(14), y + px(12), "LIVE RECEIVER", 1,
              true, true);

  const char *chip = "ON AIR";
  if (live_state == LV_CONNECTING || live_state == LV_BUFFERING)
    chip = "TUNING";
  else if (live_state == LV_STOPPED)
    chip = "STANDBY";
  else if (live_state == LV_ERROR)
    chip = "NO SIGNAL";
  int chip_h = px(22);
  int chip_w = text_w(BOBO_FONT_SMALL, chip, true) + px(20);
  int chip_x = side + w - px(14) - chip_w;
  int chip_ty = y + px(9) + (chip_h - bobo_line_height(BOBO_FONT_SMALL)) / 2;
  if (aired) {
    bobo_fill_round_rect(chip_x, y + px(9), chip_w, chip_h, px(3), true);
    bobo_draw_text(BOBO_FONT_SMALL, chip_x + px(10), chip_ty, chip, false,
                   true);
  } else {
    bobo_draw_round_rect(chip_x, y + px(9), chip_w, chip_h, 1, px(3), true);
    bobo_draw_text(BOBO_FONT_SMALL, chip_x + px(10), chip_ty, chip, true, true);
  }

  draw_radio_art(side + w / 2, y + px(76), w, art_bh, aired, channel, total);
  draw_center_col(BOBO_FONT_SMALL, y + card - px(27),
                  "HLS · AAC-LC 64K · 7 s SEGMENTS", true, false);
  y += card + px(10);

  /* Station identity: name and meta on the left, local clock on the right. */
  wall_clock_t clock;
  wall_clock(&clock);
  char hm[8], date[32];
  clock_hm(&clock, hm, sizeof(hm));
  clock_date(&clock, date, sizeof(date));
  int clock_w = px(118);
  draw_ellipsized(BOBO_FONT_TITLE, side, y, w - clock_w - px(10), name, true,
                  true);
  draw_right(BOBO_FONT_TITLE, side + w, y, hm, true, true);
  snprintf(player_line, sizeof(player_line), "%s · %s · CH %02d/%02d", sub,
           group, channel, total);
  draw_ellipsized(BOBO_FONT_SMALL, side, y + px(28), w - clock_w - px(10),
                  player_line, true, false);
  draw_right(BOBO_FONT_SMALL, side + w, y + px(28), date, true, false);
  bobo_draw_line(side, y + px(50), side + w, y + px(50), 1, true);
  y += px(58);

  int btn_h = px(56);
  draw_transport(side, y, w, btn_h);
  y += btn_h + px(10);

  draw_tune_strip(side, y, w, px(46));
  y += px(46) + px(14);

  /* Signal: state, queue and the counters that explain a dropout. */
  int sig_h = px(114);
  bobo_draw_rect(side, y, w, sig_h, 1, true);
  icon_wave(side + px(14), y + px(11), px(12), true);
  draw_spaced(BOBO_FONT_SMALL, side + px(34), y + px(10), "SIGNAL", px(2), true,
              true);
  char *route = live_stat_buf;
  if (live_audio && live_hz > 0) {
    char rn[40];
    int r = bobo_audio_route(rn, sizeof(rn));
    snprintf(route, sizeof(live_stat_buf), "%d Hz / %d ch · %s", live_hz,
             live_ch, r == BOBO_AUDIO_ROUTE_HEADSET ? "BT" : "SPK");
  } else {
    snprintf(route, sizeof(live_stat_buf), "--");
  }
  draw_right(BOBO_FONT_SMALL, side + w - px(14), y + px(10), route, true, false);

  int lx = side + px(14);
  int c2 = side + w * 45 / 100;
  int c3 = side + w * 74 / 100;
  bobo_fill_round_rect(lx, y + px(37), px(9), px(9), px(4),
                       live_state == LV_PLAYING);
  if (live_state != LV_PLAYING)
    bobo_draw_round_rect(lx, y + px(37), px(9), px(9), 1, px(4), true);
  bobo_draw_text(BOBO_FONT_BODY, lx + px(16), y + px(32), live_state_label(),
                 true, true);
  if (live_audio) {
    char *qb = live_stat_buf;
    snprintf(qb, sizeof(live_stat_buf), T("队列 %u ms", "BUFFER %u ms"),
             (unsigned)bobo_audio_buffered_ms());
    draw_right(BOBO_FONT_SMALL, side + w - px(14), y + px(36), qb, true, false);
  }
  uint32_t played_ms = 0;
  if (live_audio && live_hz > 0)
    played_ms = (uint32_t)((uint64_t)bobo_audio_played() * 1000u /
                           (uint64_t)live_hz);
  char *vb = live_stat_buf;
  snprintf(vb, sizeof(live_stat_buf), "%02u:%02u",
           (unsigned)(played_ms / 60000u), (unsigned)(played_ms / 1000u % 60u));
  stat_labelled(lx, y + px(62), T("已播", "PLAYED"), vb);
  snprintf(vb, sizeof(live_stat_buf), "%u", (unsigned)live_segs);
  stat_labelled(c2, y + px(62), T("分片", "SEG"), vb);
  snprintf(vb, sizeof(live_stat_buf), "%u", (unsigned)live_underruns);
  stat_labelled(c3, y + px(62), T("欠载", "DROPS"), vb);
  snprintf(vb, sizeof(live_stat_buf), "%u ms", (unsigned)live_fetch_ms);
  stat_labelled(lx, y + px(86), T("拉流", "FETCH"), vb);
  snprintf(vb, sizeof(live_stat_buf), "%u ms", (unsigned)live_decode_ms);
  stat_labelled(c2, y + px(86), T("解码", "DECODE"), vb);
  y += sig_h;

  if (live_note[0]) {
    int nlh = bobo_line_height(BOBO_FONT_SMALL) + px(4);
    y += px(8);
    y += draw_wrap(BOBO_FONT_SMALL, live_note, side, y, w, 2, nlh, true,
                   true) *
         nlh;
  }

  int back_h = px(48);
  int back_y = screen_h - px(96);
  if (back_y < y + px(12)) back_y = y + px(12);
  bobo_draw_rect(side, back_y, w, back_h, 1, true);
  const char *back_lbl = T("返回列表 BACK", "BACK TO LIST");
  bobo_draw_text(BOBO_FONT_BODY,
                 side + (w - text_w(BOBO_FONT_BODY, back_lbl, true)) / 2,
                 back_y + (back_h - bobo_line_height(BOBO_FONT_BODY)) / 2,
                 back_lbl, true, true);
  add_hit(side, back_y, w, back_h, ACT_LIVE_BACK, 0, 0);

  draw_center_col(BOBO_FONT_SMALL, screen_h - px(28),
                  T("音量跟随系统 · 点按换台", "SYSTEM VOLUME / TAP TO TUNE"),
                  true, false);
  present(refresh);
  live_last_ui = bobo_millis();
}

static void live_tap(int x, int y) {
  if (live_quit || live_exit) return;
  for (int i = hit_count - 1; i >= 0; i--) {
    if (!inside(&hits[i], x, y)) continue;
    int action = hits[i].action;
    if (action == ACT_LIVE_BACK) {
      live_quit = true;
    } else if (action == ACT_LIVE_PLAY) {
      if (live_on_air()) {
        live_stop_req = true;
        live_close_audio();
        live_route = -1;
        live_wd_armed = false;
        live_priming = false;
        live_prime_resume = false;
        live_state = LV_STOPPED;
        live_note[0] = '\0';
        live_draw(BOBO_REFRESH_FAST);
      } else {
        live_start_req = true;
        live_state = LV_CONNECTING;
        strcpy(live_note, T("正在重新连接直播源...",
                            "Reconnecting to the live stream..."));
        live_draw(BOBO_REFRESH_FAST);
      }
    } else if (action == ACT_LIVE_PREV || action == ACT_LIVE_NEXT) {
      live_tune = action == ACT_LIVE_NEXT ? 1 : -1;
    }
    return;
  }
}

static int live_wait_fatal(void) {
  while (!live_quit && !live_exit) live_poll();
  return live_exit ? 1 : 0;
}

static int live_target;

static void live_reset_decoder(void) {
  if (live_dec) AACFreeDecoder(live_dec);
  live_dec = AACInitDecoder();
}

/* Moves to the next/previous station inside the same group and rebuilds the
 * stream state. Returns true when the player should keep streaming. */
static bool live_switch_station(int dir) {
  int g = live_group_of(live_idx);
  int first = live_group_first[g];
  int n = live_group_n[g];
  if (n <= 1) return live_on_air();
  int pos = (live_idx - first + dir + n) % n;
  int ni = first + pos;
  bool keep = live_on_air();
  live_close_audio();
  live_route = -1;
  live_wd_armed = false;
  live_priming = false;
  live_prime_resume = false;
  live_last_prime = 0;
  live_idx = ni;
  if (!strncmp(live[ni].source, "qt:", 3))
    snprintf(live_pl_url, sizeof(live_pl_url),
             "https://ls.qingting.fm/live/%s.m3u8?bitrate=64",
             live[ni].source + 3);
  else
    snprintf(live_pl_url, sizeof(live_pl_url), "%s", live[ni].source);
  url_set_base(live_pl_url);
  live_reset_decoder();
  live_segs = live_frames = live_underruns = 0;
  live_fetch_ms = live_decode_ms = 0;
  live_hz = live_ch = 0;
  live_seg_count = 0;
  live_cur[0] = '\0';
  live_note[0] = '\0';
  live_fail_streak = 0;
  live_target = 0;
  live_last_ui = 0;
  if (keep) {
    live_state = LV_CONNECTING;
    strcpy(live_note, T("正在切换电台...", "Tuning the station..."));
    bobo_state_set("resume_live", live[ni].source);
  } else {
    live_state = LV_STOPPED;
    bobo_state_set("resume_live", "");
  }
  live_draw(BOBO_REFRESH_FAST);
  return keep;
}

/* Fetch the (possibly master) playlist and choose the segment to start at.
 * Returns false with live_state = LV_ERROR on a dead source; a user stop
 * during the attempt is reported through live_stop_req. */
static bool live_begin_stream(void) {
  live_seg_count = 0;
  live_cur[0] = '\0';
  live_note[0] = '\0';
  live_state = LV_CONNECTING;
  live_fail_streak = 0;

  if (!live_dec) {
    live_state = LV_ERROR;
    strcpy(live_note, T("解码器不可用，请返回后重试。",
                        "The decoder is unavailable; go back and retry."));
    live_draw(BOBO_REFRESH_FAST);
    return false;
  }
  if (!bobo_wifi_connected()) {
    live_state = LV_ERROR;
    strcpy(live_note, T("没有网络：连上 Wi-Fi 后点按开始。",
                        "No network: connect Wi-Fi, then press START."));
    live_draw(BOBO_REFRESH_FAST);
    return false;
  }

  strcpy(live_note, T("正在获取直播列表...", "Fetching the live playlist..."));
  live_draw(BOBO_REFRESH_FAST);
  size_t len = 0;
  uint32_t ms = 0;
  int st = 0;
  for (int tries = 0; tries < 3; tries++) {
    if (live_quit || live_exit || live_stop_req || live_tune) return false;
    st = live_fetch(live_pl_url, &len, &ms);
    if (st == 200 && len > 0) break;
    live_wait(800);
  }
  live_fetch_ms = ms;
  if (st == 200 && live_resolve_master(live_pl_url, sizeof(live_pl_url), len)) {
    strcpy(live_note,
           T("正在连接直播源...", "Connecting to the live stream..."));
    live_draw(BOBO_REFRESH_FAST);
    for (int tries = 0; tries < 3; tries++) {
      if (live_quit || live_exit || live_stop_req || live_tune) return false;
      st = live_fetch(live_pl_url, &len, &ms);
      if (st == 200 && len > 0) break;
      live_wait(800);
    }
    live_fetch_ms = ms;
  }
  if (live_tune) return false;
  if (st == 200) live_parse_playlist(len);
  if (live_seg_count == 0) {
    live_state = LV_ERROR;
    snprintf(live_note, sizeof(live_note),
             T("取不到直播列表（HTTP %d）。点按开始重试。",
               "No playlist (HTTP %d). Press START to retry."),
             st);
    live_draw(BOBO_REFRESH_FAST);
    return false;
  }
  live_target = live_seg_count >= 2 ? live_seg_count - 2 : live_seg_count - 1;
  strcpy(live_note,
         T("正在获取第一个分片...", "Fetching the first segment..."));
  live_draw(BOBO_REFRESH_FAST);
  return true;
}

static int live_run(int idx) {
  if (idx < 0 || idx >= live_count) return 0;
  live_idx = idx;
  live_quit = false;
  live_exit = false;
  live_stop_req = false;
  live_start_req = false;
  live_audio = false;
  live_priming = false;
  live_prime_resume = false;
  live_last_prime = 0;
  live_intr_t0 = 0;
  live_route = -1;
  live_route_ms = 0;
  live_wd_armed = false;
  live_segs = live_frames = live_underruns = 0;
  live_fetch_ms = live_decode_ms = 0;
  live_hz = live_ch = 0;
  live_seg_count = 0;
  live_cur[0] = '\0';
  live_note[0] = '\0';
  live_state = LV_CONNECTING;
  live_last_ui = 0;
  live_fail_streak = 0;
  live_target = 0;
  live_tune = 0;

  live_dec = AACInitDecoder();
  live_net = malloc(LIVE_NET_CAP);
  live_ts = malloc(LIVE_NET_CAP);
  if (!live_dec || !live_net || !live_ts) {
    live_state = LV_ERROR;
    strcpy(live_note, T("内存不足，无法准备直播解码。",
                        "Not enough memory to prepare the decoder."));
    live_draw(BOBO_REFRESH_HALF);
    int rc = live_wait_fatal();
    live_cleanup();
    return rc;
  }

  if (!strncmp(live[idx].source, "qt:", 3)) {
    snprintf(live_pl_url, sizeof(live_pl_url),
             "https://ls.qingting.fm/live/%s.m3u8?bitrate=64",
             live[idx].source + 3);
  } else {
    snprintf(live_pl_url, sizeof(live_pl_url), "%s", live[idx].source);
  }
  url_set_base(live_pl_url);
  bobo_state_set("resume_live", live[idx].source);

  live_draw(BOBO_REFRESH_HALF);

  bool want_play = true;
  for (;;) {
    if (live_tune) {
      int dir = live_tune;
      live_tune = 0;
      want_play = live_switch_station(dir);
      if (live_quit || live_exit) break;
    }
    bool streaming = want_play ? live_begin_stream() : false;
    if (streaming) {
      while (!live_quit && !live_exit && !live_stop_req && !live_tune) {
        live_watch_route();
        if (live_quit || live_exit || live_stop_req || live_tune) break;
        /* Refresh the window while the buffer is still full: the last listed
         * segment is about to play, so the next URL is known before the
         * buffer runs empty. */
        if (live_audio && live_segs > 0 && live_target + 1 >= live_seg_count &&
            live_fetch_ms < 3000 && bobo_audio_buffered_ms() >= 1200) {
          snprintf(live_want, sizeof(live_want), "%s", live_seg[live_target]);
          if (live_refresh_next()) {
            int pos = live_find(live_want);
            live_target =
                pos >= 0 ? pos : (live_seg_count >= 2 ? live_seg_count - 2 : 0);
          }
        }
        if (!live_play_segment(live_seg[live_target])) {
          if (live_quit || live_exit || live_stop_req || live_tune) break;
          live_fail_streak++;
          if (live_fail_streak >= 5) {
            live_state = LV_ERROR;
            strcpy(live_note,
                   T("连续取流失败，点按开始重试。",
                     "Stream failed five times. Press START to retry."));
            live_draw(BOBO_REFRESH_FAST);
            break;
          }
          live_state = LV_BUFFERING;
          if (!live_note[0])
            strcpy(live_note, T("网络不稳定，正在重试...",
                                "Network unstable, retrying..."));
          live_draw(BOBO_REFRESH_FAST);
          live_wait(800);
          continue;
        }
        live_fail_streak = 0;
        strcpy(live_cur, live_seg[live_target]);
        if (live_segs == 0) {
          remember_live(live_idx);
          recent_live_add(live_idx);
        }
        live_segs++;
        if (live_audio) live_note[0] = '\0';
        if (live_audio && live_state != LV_INTERRUPTED)
          live_state = LV_PLAYING;
        if (live_last_ui == 0 || bobo_millis() - live_last_ui >= 30000)
          live_draw(BOBO_REFRESH_FAST);

        if (live_target + 1 < live_seg_count) {
          live_target++;
        } else if (live_refresh_next()) {
          int pos = live_find(live_cur);
          live_target = pos >= 0 && pos + 1 < live_seg_count
                            ? pos + 1
                            : (live_seg_count > 0 ? live_seg_count - 1 : 0);
        } else {
          live_target = live_seg_count > 0 ? live_seg_count - 1 : 0;
        }
      }
    }
    if (live_quit || live_exit) break;
    if (live_tune) continue;

    if (live_stop_req) {
      live_stop_req = false;
      live_close_audio();
      bobo_state_set("resume_live", "");
      live_state = LV_STOPPED;
      live_note[0] = '\0';
      live_draw(BOBO_REFRESH_FAST);
    }
    want_play = false;

    /* Idle on the player page: the clock keeps ticking and START/BACK stay
     * responsive without any network or audio work. */
    while (!live_quit && !live_exit && !live_start_req && !live_tune) {
      live_poll();
      if (live_stop_req) live_stop_req = false;
      if (bobo_millis() - live_last_ui >= 30000) live_draw(BOBO_REFRESH_FAST);
      bobo_delay(30);
    }
    if (live_quit || live_exit) break;
    if (live_tune) continue;
    live_start_req = false;
    want_play = true;
    bobo_state_set("resume_live", live[live_idx].source);

    live_reset_decoder();
    if (!live_dec) {
      live_state = LV_ERROR;
      strcpy(live_note, T("无法重新初始化解码器，请返回后重试。",
                          "Could not restart the decoder; go back and retry."));
      live_draw(BOBO_REFRESH_FAST);
      continue;
    }
    live_segs = live_frames = live_underruns = 0;
    live_fetch_ms = live_decode_ms = 0;
    live_hz = live_ch = 0;
    live_state = LV_CONNECTING;
    strcpy(live_note, T("正在重新连接直播源...",
                        "Reconnecting to the live stream..."));
    live_draw(BOBO_REFRESH_FAST);
  }

  /* Leave the station list scrolled to the station that played last. */
  bool listed_here =
      page_arg == -1 ? live_group_is_cn(live[live_idx].group)
                     : page_arg == live_group_of(live_idx);
  if (!live_exit && page == PG_LIVE_ST && live_idx >= 0 && page_rows > 0 &&
      listed_here) {
    int base = page_arg == -1 ? 0 : live_group_first[page_arg];
    int pos = live_idx - base;
    if (pos >= 0) list_first = (pos / page_rows) * page_rows;
  }

  bobo_log("live: stop segs=%u frames=%u underruns=%u", (unsigned)live_segs,
           (unsigned)live_frames, (unsigned)live_underruns);
  bool user_quit = live_quit;
  if (!live_exit) {
    live_drain_input();
    if (!live_exit) back_guard_until = bobo_millis() + 700;
  }
  if (user_quit || !live_exit) bobo_state_set("resume_live", "");
  live_cleanup();
  return live_exit ? 1 : 0;
}

/* ---- navigation --------------------------------------------------------- */

static bool valid_page(int p, int a) {
  switch (p) {
    case PG_HOME:
    case PG_FAVS:
    case PG_RECENT:
    case PG_LIVE:
    case PG_ABOUT:
      return true;
    case PG_LIVE_ST:
      return a >= -1 && a < live_group_count;
    default:
      return false;
  }
}

static void save_nav(void) {
  char *b = nav_state_buf;
  int len = snprintf(b, sizeof(nav_state_buf), "%d,%d,%d,%d", page, page_arg,
                     list_first, nav_depth);
  for (int i = 0; i < nav_depth && len > 0 &&
                  len < (int)sizeof(nav_state_buf) - 10;
       i++)
    len += snprintf(b + len, sizeof(nav_state_buf) - (size_t)len, ",%d,%d,%d",
                    nav_page[i], nav_arg[i], nav_list[i]);
  bobo_state_set("nav_live", b);
}

static void clear_nav(void) { bobo_state_set("nav_live", ""); }

static int parse_nav_ints(const char *s, int *out, int max) {
  int n = 0;
  while (*s && n < max) {
    while (*s && (*s < '0' || *s > '9')) s++;
    if (!*s) break;
    int v = 0;
    while (*s >= '0' && *s <= '9') {
      if (v < 100000) v = v * 10 + (*s - '0');
      s++;
    }
    out[n++] = v;
  }
  return n;
}

static void restore_nav(void) {
  char *b = nav_state_buf;
  if (bobo_state_get("nav_live", b, sizeof(nav_state_buf)) <= 0) return;
  int v[4 + MAX_NAV * 3];
  int n = parse_nav_ints(b, v, (int)(sizeof(v) / sizeof(v[0])));
  if (n < 4) return;
  int depth = v[3];
  if (depth < 0 || depth > MAX_NAV || n < 4 + depth * 3) return;
  if (!valid_page(v[0], v[1])) return;
  for (int i = 0; i < depth; i++) {
    int p = v[4 + i * 3], a = v[5 + i * 3];
    if (!valid_page(p, a)) return;
  }
  page = v[0];
  page_arg = v[1];
  list_first = v[2] > 0 ? v[2] : 0;
  nav_depth = depth;
  for (int i = 0; i < depth; i++) {
    nav_page[i] = v[4 + i * 3];
    nav_arg[i] = v[5 + i * 3];
    nav_list[i] = v[6 + i * 3];
  }
  bobo_log("world-radio: restored page %d arg %d depth %d", page, page_arg,
           nav_depth);
}

static void nav(int p, int a) {
  if (nav_depth < MAX_NAV) {
    nav_page[nav_depth] = page;
    nav_arg[nav_depth] = page_arg;
    nav_list[nav_depth] = list_first;
    nav_depth++;
  }
  page = p;
  page_arg = a;
  list_first = 0;
  dirty = true;
  save_nav();
  bobo_log("nav page %d arg %d", p, a);
}

static bool go_back(void) {
  if (nav_depth == 0) return false;
  nav_depth--;
  page = nav_page[nav_depth];
  page_arg = nav_arg[nav_depth];
  list_first = nav_list[nav_depth];
  dirty = true;
  save_nav();
  return true;
}

static void page_shift(int dir) {
  if (page != PG_FAVS && page != PG_RECENT && page != PG_LIVE &&
      page != PG_LIVE_ST)
    return;
  if (page_total <= page_rows) return;
  int max_first = ((page_total - 1) / page_rows) * page_rows;
  int next = list_first + dir * page_rows;
  if (next < 0) next = 0;
  if (next > max_first) next = max_first;
  if (next != list_first) {
    list_first = next;
    dirty = true;
  }
}

static void do_action(const hit_t *h) {
  switch (h->action) {
    case ACT_FAVS:
      nav(PG_FAVS, 0);
      break;
    case ACT_RECENT:
      nav(PG_RECENT, 0);
      break;
    case ACT_LIVE_FAV:
      live_fav_toggle(h->a);
      dirty = true;
      break;
    case ACT_LIVE_FILTER: {
      live_filter = h->a;
      char fb[4];
      snprintf(fb, sizeof(fb), "%d", live_filter);
      bobo_state_set("live_filter", fb);
      list_first = 0;
      if (page == PG_HOME)
        nav(PG_LIVE, 0);
      else
        dirty = true;
      break;
    }
    case ACT_LIVE:
      nav(PG_LIVE, 0);
      break;
    case ACT_LIVE_GROUP:
      nav(PG_LIVE_ST, h->a);
      break;
    case ACT_LIVE_STATION:
      if (live_run(h->a)) app_should_exit = true;
      dirty = true;
      break;
    case ACT_ABOUT:
      nav(PG_ABOUT, 0);
      break;
    case ACT_LANG_AUTO:
    case ACT_LANG_ZH:
    case ACT_LANG_EN: {
      int mode = h->action == ACT_LANG_ZH
                     ? 1
                     : (h->action == ACT_LANG_EN ? 2 : 0);
      set_language_mode(mode);
      dirty = true;
      break;
    }
    case ACT_RESUME: {
      int li = last_live_index();
      if (li >= 0) {
        nav(PG_LIVE_ST, live_group_of(li));
        if (live_run(li)) app_should_exit = true;
      }
      dirty = true;
      break;
    }
    case ACT_PAGE:
      page_shift(h->a);
      break;
    default:
      break;
  }
}

int app_main(int argc, char *argv[]) {
  (void)argc;
  screen_w = bobo_screen_width();
  screen_h = bobo_screen_height();
  bobo_log("world-radio: start in %s", argv[1]);
  live_ok = load_live();
  load_state();
  load_language();
  load_last();
  if (!live_ok)
    bobo_log("world-radio: live.tsv missing");
  else
    restore_nav();
  bobo_log("world-radio: live stations %d in %d groups", live_count,
           live_group_count);

  if (live_ok) {
    int rl = resume_live_index();
    if (rl >= 0) {
      bobo_log("world-radio: resume live %d", rl);
      page = PG_HOME;
      nav_depth = 0;
      nav(PG_LIVE_ST, live_group_of(rl));
      if (live_run(rl)) app_should_exit = true;
    }
  }

  for (;;) {
    if (app_should_exit) {
      save_nav();
      return 0;
    }
    if (dirty) {
      int mode = first_draw ? BOBO_REFRESH_HALF : BOBO_REFRESH_FAST;
      draw_page(mode);
      first_draw = false;
      dirty = false;
    }
    bobo_event_t ev;
    if (!bobo_wait_event(&ev, 60000)) continue;
    switch (ev.type) {
      case BOBO_EV_EXIT:
        save_nav();
        return 0;
      case BOBO_EV_REDRAW:
        draw_page(BOBO_REFRESH_HALF);
        break;
      case BOBO_EV_BUTTON:
        if (ev.button == BOBO_BTN_BACK) {
          if ((int32_t)(back_guard_until - bobo_millis()) > 0) break;
          if (!go_back()) {
            clear_nav();
            return 0;
          }
        } else if (ev.button == BOBO_BTN_UP || ev.button == BOBO_BTN_LEFT) {
          page_shift(-1);
        } else if (ev.button == BOBO_BTN_DOWN ||
                   ev.button == BOBO_BTN_RIGHT) {
          page_shift(1);
        }
        break;
      case BOBO_EV_SWIPE:
        if (ev.dy < 0)
          page_shift(1);
        else if (ev.dy > 0)
          page_shift(-1);
        break;
      case BOBO_EV_TAP:
        for (int i = hit_count - 1; i >= 0; i--) {
          if (!inside(&hits[i], ev.x, ev.y)) continue;
          do_action(&hits[i]);
          break;
        }
        break;
      default:
        break;
    }
  }
}