/****************************************************************************
 * apps/system/desktop/qpk_canvas.c
 *
 * CanvasRenderingContext2D subset used by Quick App <canvas>.
 * Commands are replayed in LV_EVENT_DRAW_MAIN — no pixel buffer.
 ****************************************************************************/

#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <lvgl/lvgl.h>
#include <quickjs.h>

#include "qpk_canvas.h"
#include "qpk_priv.h"

#ifndef M_PI
#  define M_PI  3.14159265358979323846
#endif

#define QPK_CV_MAX    2
#define QPK_CV_CMDS   160
#define QPK_CV_PTS    96

enum qpk_cv_op_e
{
  QPK_CV_FILL_ELLIPSE = 0,
  QPK_CV_STROKE_ELLIPSE,
  QPK_CV_FILL_RECT,
  QPK_CV_STROKE_RECT,
  QPK_CV_LINE,
  QPK_CV_FILL_TRI
};

enum qpk_cv_path_e
{
  QPK_CV_PATH_EMPTY = 0,
  QPK_CV_PATH_ELLIPSE,
  QPK_CV_PATH_POLY
};

struct qpk_cv_cmd_s
{
  uint8_t op;
  uint8_t opa;
  uint16_t width;
  uint32_t color;
  int16_t a;
  int16_t b;
  int16_t c;
  int16_t d;
  int16_t e;
  int16_t f;
};

struct qpk_cv_s
{
  lv_obj_t *obj;
  int handle;
  bool used;
  struct qpk_cv_cmd_s cmds[QPK_CV_CMDS];
  int ncmd;
  int16_t px[QPK_CV_PTS];
  int16_t py[QPK_CV_PTS];
  int npts;
  int16_t ell_x;
  int16_t ell_y;
  int16_t ell_rx;
  int16_t ell_ry;
  uint8_t path;
  uint8_t closed;
};

static struct qpk_cv_s g_cv[QPK_CV_MAX];

static int i16(double v)
{
  if (v > 32767)
    {
      return 32767;
    }

  if (v < -32768)
    {
      return -32768;
    }

  return (int)lround(v);
}

static uint32_t css_color(JSContext *ctx, JSValue v)
{
  uint32_t n = 0;
  const char *s;
  size_t len;

  if (JS_IsNumber(v))
    {
      if (JS_ToUint32(ctx, &n, v) == 0)
        {
          return n & 0xffffffu;
        }
    }

  s = JS_ToCString(ctx, v);
  if (s == NULL)
    {
      return 0;
    }

  len = strlen(s);
  if (s[0] == '#' && len == 7)
    {
      n = (uint32_t)strtoul(s + 1, NULL, 16);
    }
  else if (s[0] == '#' && len == 4)
    {
      unsigned r = (s[1] >= 'a' ? s[1] - 'a' + 10 :
                    s[1] >= 'A' ? s[1] - 'A' + 10 : s[1] - '0') & 15;
      unsigned g = (s[2] >= 'a' ? s[2] - 'a' + 10 :
                    s[2] >= 'A' ? s[2] - 'A' + 10 : s[2] - '0') & 15;
      unsigned b = (s[3] >= 'a' ? s[3] - 'a' + 10 :
                    s[3] >= 'A' ? s[3] - 'A' + 10 : s[3] - '0') & 15;
      n = (r << 20) | (r << 16) | (g << 12) | (g << 8) | (b << 4) | b;
    }

  JS_FreeCString(ctx, s);
  return n & 0xffffffu;
}

static void ctx_style(JSContext *ctx, JSValueConst this_val, int fill,
                      uint32_t *color, uint8_t *opa, int16_t *lw)
{
  JSValue v;
  double n;

  v = JS_GetPropertyStr(ctx, this_val, fill ? "fillStyle" : "strokeStyle");
  *color = css_color(ctx, v);
  JS_FreeValue(ctx, v);

  n = 1;
  v = JS_GetPropertyStr(ctx, this_val, "globalAlpha");
  JS_ToFloat64(ctx, &n, v);
  JS_FreeValue(ctx, v);
  if (n < 0)
    {
      n = 0;
    }

  if (n > 1)
    {
      n = 1;
    }

  *opa = (uint8_t)lround(n * 255.0);

  if (lw != NULL)
    {
      n = 1;
      v = JS_GetPropertyStr(ctx, this_val, "lineWidth");
      JS_ToFloat64(ctx, &n, v);
      JS_FreeValue(ctx, v);
      if (n < 1)
        {
          n = 1;
        }

      if (n > 64)
        {
          n = 64;
        }

      *lw = (int16_t)lround(n);
    }
}

static struct qpk_cv_s *cv_from_this(JSContext *ctx, JSValueConst this_val)
{
  JSValue h;
  int32_t handle = 0;
  int i;

  h = JS_GetPropertyStr(ctx, this_val, "handle");
  JS_ToInt32(ctx, &handle, h);
  JS_FreeValue(ctx, h);
  for (i = 0; i < QPK_CV_MAX; i++)
    {
      if (g_cv[i].used && g_cv[i].handle == handle)
        {
          return &g_cv[i];
        }
    }

  return NULL;
}

static void cv_add_pt(struct qpk_cv_s *cv, double x, double y)
{
  if (cv->npts >= QPK_CV_PTS)
    {
      return;
    }

  cv->px[cv->npts] = (int16_t)i16(x);
  cv->py[cv->npts] = (int16_t)i16(y);
  cv->npts++;
}

static void cv_add_cmd(struct qpk_cv_s *cv, uint8_t op, uint32_t color,
                       uint8_t opa, int16_t width,
                       int16_t a, int16_t b, int16_t c, int16_t d,
                       int16_t e, int16_t f)
{
  struct qpk_cv_cmd_s *cmd;

  if (cv->ncmd >= QPK_CV_CMDS)
    {
      return;
    }

  cmd = &cv->cmds[cv->ncmd++];
  cmd->op = op;
  cmd->opa = opa;
  cmd->width = (uint16_t)width;
  cmd->color = color;
  cmd->a = a;
  cmd->b = b;
  cmd->c = c;
  cmd->d = d;
  cmd->e = e;
  cmd->f = f;
}

static void cv_sample_ellipse(struct qpk_cv_s *cv, double x, double y,
                              double rx, double ry,
                              double a0, double a1, int ccw)
{
  double span;
  int steps;
  int i;

  span = a1 - a0;
  if (ccw)
    {
      if (span > 0)
        {
          span -= 2 * M_PI;
        }
    }
  else if (span < 0)
    {
      span += 2 * M_PI;
    }

  steps = (int)lround(fabs(span) / (M_PI / 12.0));
  if (steps < 8)
    {
      steps = 8;
    }

  if (steps > 32)
    {
      steps = 32;
    }

  for (i = 0; i <= steps; i++)
    {
      double a = a0 + span * (double)i / (double)steps;
      cv_add_pt(cv, x + rx * cos(a), y + ry * sin(a));
    }
}

static void cv_draw(lv_event_t *event)
{
  struct qpk_cv_s *cv = lv_event_get_user_data(event);
  lv_layer_t *layer;
  lv_area_t coords;
  int ox;
  int oy;
  int i;

  if (cv == NULL || !cv->used)
    {
      return;
    }

  layer = lv_event_get_layer(event);
  if (layer == NULL)
    {
      return;
    }

  lv_obj_get_coords(cv->obj, &coords);
  ox = coords.x1;
  oy = coords.y1;

  for (i = 0; i < cv->ncmd; i++)
    {
      const struct qpk_cv_cmd_s *cmd = &cv->cmds[i];
      lv_color_t color = lv_color_hex(cmd->color);

      if (cmd->op == QPK_CV_FILL_ELLIPSE || cmd->op == QPK_CV_STROKE_ELLIPSE ||
          cmd->op == QPK_CV_FILL_RECT || cmd->op == QPK_CV_STROKE_RECT)
        {
          lv_draw_rect_dsc_t dsc;
          lv_area_t area;
          int x1;
          int y1;
          int x2;
          int y2;

          lv_draw_rect_dsc_init(&dsc);
          if (cmd->op == QPK_CV_FILL_ELLIPSE || cmd->op == QPK_CV_STROKE_ELLIPSE)
            {
              x1 = ox + cmd->a - cmd->c;
              y1 = oy + cmd->b - cmd->d;
              x2 = ox + cmd->a + cmd->c;
              y2 = oy + cmd->b + cmd->d;
              dsc.radius = LV_RADIUS_CIRCLE;
            }
          else
            {
              x1 = ox + cmd->a;
              y1 = oy + cmd->b;
              x2 = x1 + cmd->c;
              y2 = y1 + cmd->d;
              dsc.radius = 0;
            }

          if (x2 <= x1)
            {
              x2 = x1 + 1;
            }

          if (y2 <= y1)
            {
              y2 = y1 + 1;
            }

          lv_area_set(&area, x1, y1, x2 - 1, y2 - 1);
          if (cmd->op == QPK_CV_FILL_ELLIPSE || cmd->op == QPK_CV_FILL_RECT)
            {
              dsc.bg_color = color;
              dsc.bg_opa = cmd->opa;
              dsc.border_opa = LV_OPA_TRANSP;
            }
          else
            {
              dsc.bg_opa = LV_OPA_TRANSP;
              dsc.border_color = color;
              dsc.border_opa = cmd->opa;
              dsc.border_width = cmd->width;
            }

          lv_draw_rect(layer, &dsc, &area);
        }
      else if (cmd->op == QPK_CV_LINE)
        {
          lv_draw_line_dsc_t dsc;

          lv_draw_line_dsc_init(&dsc);
          dsc.color = color;
          dsc.opa = cmd->opa;
          dsc.width = cmd->width;
          dsc.round_start = 1;
          dsc.round_end = 1;
          dsc.p1.x = ox + cmd->a;
          dsc.p1.y = oy + cmd->b;
          dsc.p2.x = ox + cmd->c;
          dsc.p2.y = oy + cmd->d;
          lv_draw_line(layer, &dsc);
        }
      else if (cmd->op == QPK_CV_FILL_TRI)
        {
          lv_draw_triangle_dsc_t dsc;

          lv_draw_triangle_dsc_init(&dsc);
          dsc.bg_color = color;
          dsc.bg_opa = cmd->opa;
          dsc.p[0].x = ox + cmd->a;
          dsc.p[0].y = oy + cmd->b;
          dsc.p[1].x = ox + cmd->c;
          dsc.p[1].y = oy + cmd->d;
          dsc.p[2].x = ox + cmd->e;
          dsc.p[2].y = oy + cmd->f;
          lv_draw_triangle(layer, &dsc);
        }
    }
}

static void cv_deleted(lv_event_t *event)
{
  struct qpk_cv_s *cv = lv_event_get_user_data(event);

  if (cv != NULL)
    {
      memset(cv, 0, sizeof(*cv));
    }
}

static JSValue js_begin_path(JSContext *ctx, JSValueConst this_val,
                             int argc, JSValueConst *argv)
{
  struct qpk_cv_s *cv = cv_from_this(ctx, this_val);

  (void)argc;
  (void)argv;
  if (cv == NULL)
    {
      return JS_UNDEFINED;
    }

  cv->npts = 0;
  cv->path = QPK_CV_PATH_EMPTY;
  cv->closed = 0;
  return JS_UNDEFINED;
}

static JSValue js_close_path(JSContext *ctx, JSValueConst this_val,
                             int argc, JSValueConst *argv)
{
  struct qpk_cv_s *cv = cv_from_this(ctx, this_val);

  (void)argc;
  (void)argv;
  if (cv == NULL || cv->npts < 1)
    {
      return JS_UNDEFINED;
    }

  cv_add_pt(cv, cv->px[0], cv->py[0]);
  cv->closed = 1;
  return JS_UNDEFINED;
}

static JSValue js_move_to(JSContext *ctx, JSValueConst this_val,
                          int argc, JSValueConst *argv)
{
  struct qpk_cv_s *cv = cv_from_this(ctx, this_val);
  double x = 0;
  double y = 0;

  if (cv == NULL)
    {
      return JS_UNDEFINED;
    }

  JS_ToFloat64(ctx, &x, argc > 0 ? argv[0] : JS_UNDEFINED);
  JS_ToFloat64(ctx, &y, argc > 1 ? argv[1] : JS_UNDEFINED);
  cv->path = QPK_CV_PATH_POLY;
  cv_add_pt(cv, x, y);
  return JS_UNDEFINED;
}

static JSValue js_line_to(JSContext *ctx, JSValueConst this_val,
                          int argc, JSValueConst *argv)
{
  struct qpk_cv_s *cv = cv_from_this(ctx, this_val);
  double x = 0;
  double y = 0;

  if (cv == NULL)
    {
      return JS_UNDEFINED;
    }

  JS_ToFloat64(ctx, &x, argc > 0 ? argv[0] : JS_UNDEFINED);
  JS_ToFloat64(ctx, &y, argc > 1 ? argv[1] : JS_UNDEFINED);
  cv->path = QPK_CV_PATH_POLY;
  cv_add_pt(cv, x, y);
  return JS_UNDEFINED;
}

static JSValue js_arc(JSContext *ctx, JSValueConst this_val,
                      int argc, JSValueConst *argv)
{
  struct qpk_cv_s *cv = cv_from_this(ctx, this_val);
  double x = 0;
  double y = 0;
  double r = 0;
  double a0 = 0;
  double a1 = 0;
  int ccw = 0;
  double span;

  if (cv == NULL)
    {
      return JS_UNDEFINED;
    }

  JS_ToFloat64(ctx, &x, argc > 0 ? argv[0] : JS_UNDEFINED);
  JS_ToFloat64(ctx, &y, argc > 1 ? argv[1] : JS_UNDEFINED);
  JS_ToFloat64(ctx, &r, argc > 2 ? argv[2] : JS_UNDEFINED);
  JS_ToFloat64(ctx, &a0, argc > 3 ? argv[3] : JS_UNDEFINED);
  JS_ToFloat64(ctx, &a1, argc > 4 ? argv[4] : JS_UNDEFINED);
  if (argc > 5)
    {
      ccw = JS_ToBool(ctx, argv[5]);
    }

  if (r < 0)
    {
      r = 0;
    }

  span = fabs(a1 - a0);
  if (cv->path == QPK_CV_PATH_EMPTY && span >= 2 * M_PI - 0.05)
    {
      cv->path = QPK_CV_PATH_ELLIPSE;
      cv->ell_x = (int16_t)i16(x);
      cv->ell_y = (int16_t)i16(y);
      cv->ell_rx = (int16_t)i16(r);
      cv->ell_ry = (int16_t)i16(r);
    }
  else
    {
      cv->path = QPK_CV_PATH_POLY;
    }

  cv_sample_ellipse(cv, x, y, r, r, a0, a1, ccw);
  return JS_UNDEFINED;
}

static JSValue js_ellipse(JSContext *ctx, JSValueConst this_val,
                          int argc, JSValueConst *argv)
{
  struct qpk_cv_s *cv = cv_from_this(ctx, this_val);
  double x = 0;
  double y = 0;
  double rx = 0;
  double ry = 0;
  double a0 = 0;
  double a1 = 2 * M_PI;
  int ccw = 0;
  double span;

  if (cv == NULL)
    {
      return JS_UNDEFINED;
    }

  JS_ToFloat64(ctx, &x, argc > 0 ? argv[0] : JS_UNDEFINED);
  JS_ToFloat64(ctx, &y, argc > 1 ? argv[1] : JS_UNDEFINED);
  JS_ToFloat64(ctx, &rx, argc > 2 ? argv[2] : JS_UNDEFINED);
  JS_ToFloat64(ctx, &ry, argc > 3 ? argv[3] : JS_UNDEFINED);
  if (argc > 5)
    {
      JS_ToFloat64(ctx, &a0, argv[5]);
    }

  if (argc > 6)
    {
      JS_ToFloat64(ctx, &a1, argv[6]);
    }

  if (argc > 7)
    {
      ccw = JS_ToBool(ctx, argv[7]);
    }

  if (rx < 0)
    {
      rx = 0;
    }

  if (ry < 0)
    {
      ry = 0;
    }

  span = fabs(a1 - a0);
  if (cv->path == QPK_CV_PATH_EMPTY && span >= 2 * M_PI - 0.05)
    {
      cv->path = QPK_CV_PATH_ELLIPSE;
      cv->ell_x = (int16_t)i16(x);
      cv->ell_y = (int16_t)i16(y);
      cv->ell_rx = (int16_t)i16(rx);
      cv->ell_ry = (int16_t)i16(ry);
    }
  else
    {
      cv->path = QPK_CV_PATH_POLY;
    }

  cv_sample_ellipse(cv, x, y, rx, ry, a0, a1, ccw);
  return JS_UNDEFINED;
}

static JSValue js_quad_to(JSContext *ctx, JSValueConst this_val,
                          int argc, JSValueConst *argv)
{
  struct qpk_cv_s *cv = cv_from_this(ctx, this_val);
  double x0;
  double y0;
  double x1 = 0;
  double y1 = 0;
  double x2 = 0;
  double y2 = 0;
  int i;

  if (cv == NULL || cv->npts < 1)
    {
      return JS_UNDEFINED;
    }

  x0 = cv->px[cv->npts - 1];
  y0 = cv->py[cv->npts - 1];
  JS_ToFloat64(ctx, &x1, argc > 0 ? argv[0] : JS_UNDEFINED);
  JS_ToFloat64(ctx, &y1, argc > 1 ? argv[1] : JS_UNDEFINED);
  JS_ToFloat64(ctx, &x2, argc > 2 ? argv[2] : JS_UNDEFINED);
  JS_ToFloat64(ctx, &y2, argc > 3 ? argv[3] : JS_UNDEFINED);
  cv->path = QPK_CV_PATH_POLY;
  for (i = 1; i <= 8; i++)
    {
      double t = (double)i / 8.0;
      double u = 1.0 - t;
      cv_add_pt(cv, u * u * x0 + 2 * u * t * x1 + t * t * x2,
                u * u * y0 + 2 * u * t * y1 + t * t * y2);
    }

  return JS_UNDEFINED;
}

static JSValue js_cubic_to(JSContext *ctx, JSValueConst this_val,
                           int argc, JSValueConst *argv)
{
  struct qpk_cv_s *cv = cv_from_this(ctx, this_val);
  double x0;
  double y0;
  double x1 = 0;
  double y1 = 0;
  double x2 = 0;
  double y2 = 0;
  double x3 = 0;
  double y3 = 0;
  int i;

  if (cv == NULL || cv->npts < 1)
    {
      return JS_UNDEFINED;
    }

  x0 = cv->px[cv->npts - 1];
  y0 = cv->py[cv->npts - 1];
  JS_ToFloat64(ctx, &x1, argc > 0 ? argv[0] : JS_UNDEFINED);
  JS_ToFloat64(ctx, &y1, argc > 1 ? argv[1] : JS_UNDEFINED);
  JS_ToFloat64(ctx, &x2, argc > 2 ? argv[2] : JS_UNDEFINED);
  JS_ToFloat64(ctx, &y2, argc > 3 ? argv[3] : JS_UNDEFINED);
  JS_ToFloat64(ctx, &x3, argc > 4 ? argv[4] : JS_UNDEFINED);
  JS_ToFloat64(ctx, &y3, argc > 5 ? argv[5] : JS_UNDEFINED);
  cv->path = QPK_CV_PATH_POLY;
  for (i = 1; i <= 10; i++)
    {
      double t = (double)i / 10.0;
      double u = 1.0 - t;
      double x = u * u * u * x0 + 3 * u * u * t * x1 +
                 3 * u * t * t * x2 + t * t * t * x3;
      double y = u * u * u * y0 + 3 * u * u * t * y1 +
                 3 * u * t * t * y2 + t * t * t * y3;
      cv_add_pt(cv, x, y);
    }

  return JS_UNDEFINED;
}

static JSValue js_fill(JSContext *ctx, JSValueConst this_val,
                       int argc, JSValueConst *argv)
{
  struct qpk_cv_s *cv = cv_from_this(ctx, this_val);
  uint32_t color;
  uint8_t opa;
  int i;
  int last;

  (void)argc;
  (void)argv;
  if (cv == NULL)
    {
      return JS_UNDEFINED;
    }

  ctx_style(ctx, this_val, 1, &color, &opa, NULL);
  if (cv->path == QPK_CV_PATH_ELLIPSE)
    {
      cv_add_cmd(cv, QPK_CV_FILL_ELLIPSE, color, opa, 0,
                 cv->ell_x, cv->ell_y, cv->ell_rx, cv->ell_ry, 0, 0);
    }
  else if (cv->npts >= 3)
    {
      last = cv->npts - (cv->closed ? 1 : 0);
      for (i = 1; i + 1 < last; i++)
        {
          cv_add_cmd(cv, QPK_CV_FILL_TRI, color, opa, 0,
                     cv->px[0], cv->py[0],
                     cv->px[i], cv->py[i],
                     cv->px[i + 1], cv->py[i + 1]);
        }
    }

  lv_obj_invalidate(cv->obj);
  return JS_UNDEFINED;
}

static JSValue js_stroke(JSContext *ctx, JSValueConst this_val,
                         int argc, JSValueConst *argv)
{
  struct qpk_cv_s *cv = cv_from_this(ctx, this_val);
  uint32_t color;
  uint8_t opa;
  int16_t lw;
  int i;

  (void)argc;
  (void)argv;
  if (cv == NULL)
    {
      return JS_UNDEFINED;
    }

  ctx_style(ctx, this_val, 0, &color, &opa, &lw);
  if (cv->path == QPK_CV_PATH_ELLIPSE)
    {
      cv_add_cmd(cv, QPK_CV_STROKE_ELLIPSE, color, opa, lw,
                 cv->ell_x, cv->ell_y, cv->ell_rx, cv->ell_ry, 0, 0);
    }
  else
    {
      for (i = 1; i < cv->npts; i++)
        {
          cv_add_cmd(cv, QPK_CV_LINE, color, opa, lw,
                     cv->px[i - 1], cv->py[i - 1],
                     cv->px[i], cv->py[i], 0, 0);
        }
    }

  lv_obj_invalidate(cv->obj);
  return JS_UNDEFINED;
}

static JSValue js_fill_rect(JSContext *ctx, JSValueConst this_val,
                            int argc, JSValueConst *argv)
{
  struct qpk_cv_s *cv = cv_from_this(ctx, this_val);
  uint32_t color;
  uint8_t opa;
  double x = 0;
  double y = 0;
  double w = 0;
  double h = 0;

  if (cv == NULL)
    {
      return JS_UNDEFINED;
    }

  JS_ToFloat64(ctx, &x, argc > 0 ? argv[0] : JS_UNDEFINED);
  JS_ToFloat64(ctx, &y, argc > 1 ? argv[1] : JS_UNDEFINED);
  JS_ToFloat64(ctx, &w, argc > 2 ? argv[2] : JS_UNDEFINED);
  JS_ToFloat64(ctx, &h, argc > 3 ? argv[3] : JS_UNDEFINED);
  ctx_style(ctx, this_val, 1, &color, &opa, NULL);
  cv_add_cmd(cv, QPK_CV_FILL_RECT, color, opa, 0,
             (int16_t)i16(x), (int16_t)i16(y),
             (int16_t)i16(w), (int16_t)i16(h), 0, 0);
  lv_obj_invalidate(cv->obj);
  return JS_UNDEFINED;
}

static JSValue js_stroke_rect(JSContext *ctx, JSValueConst this_val,
                              int argc, JSValueConst *argv)
{
  struct qpk_cv_s *cv = cv_from_this(ctx, this_val);
  uint32_t color;
  uint8_t opa;
  int16_t lw;
  double x = 0;
  double y = 0;
  double w = 0;
  double h = 0;

  if (cv == NULL)
    {
      return JS_UNDEFINED;
    }

  JS_ToFloat64(ctx, &x, argc > 0 ? argv[0] : JS_UNDEFINED);
  JS_ToFloat64(ctx, &y, argc > 1 ? argv[1] : JS_UNDEFINED);
  JS_ToFloat64(ctx, &w, argc > 2 ? argv[2] : JS_UNDEFINED);
  JS_ToFloat64(ctx, &h, argc > 3 ? argv[3] : JS_UNDEFINED);
  ctx_style(ctx, this_val, 0, &color, &opa, &lw);
  cv_add_cmd(cv, QPK_CV_STROKE_RECT, color, opa, lw,
             (int16_t)i16(x), (int16_t)i16(y),
             (int16_t)i16(w), (int16_t)i16(h), 0, 0);
  lv_obj_invalidate(cv->obj);
  return JS_UNDEFINED;
}

static JSValue js_clear_rect(JSContext *ctx, JSValueConst this_val,
                             int argc, JSValueConst *argv)
{
  struct qpk_cv_s *cv = cv_from_this(ctx, this_val);

  (void)argc;
  (void)argv;
  if (cv == NULL)
    {
      return JS_UNDEFINED;
    }

  cv->ncmd = 0;
  lv_obj_invalidate(cv->obj);
  return JS_UNDEFINED;
}

static JSValue js_ui_canvas(JSContext *ctx, JSValueConst this_val,
                            int argc, JSValueConst *argv)
{
  struct qpk_cv_s *cv = NULL;
  lv_obj_t *obj;
  JSValue out;
  int32_t x = 0;
  int32_t y = 0;
  int32_t w = 100;
  int32_t h = 100;
  int i;
  int handle;

  (void)this_val;
  if (qpk_root_obj() == NULL)
    {
      return JS_ThrowInternalError(ctx, "no page");
    }

  if (argc > 0)
    {
      JS_ToInt32(ctx, &x, argv[0]);
    }

  if (argc > 1)
    {
      JS_ToInt32(ctx, &y, argv[1]);
    }

  if (argc > 2)
    {
      JS_ToInt32(ctx, &w, argv[2]);
    }

  if (argc > 3)
    {
      JS_ToInt32(ctx, &h, argv[3]);
    }

  if (w < 1)
    {
      w = 1;
    }

  if (h < 1)
    {
      h = 1;
    }

  for (i = 0; i < QPK_CV_MAX; i++)
    {
      if (!g_cv[i].used)
        {
          cv = &g_cv[i];
          break;
        }
    }

  if (cv == NULL)
    {
      return JS_ThrowInternalError(ctx, "too many canvases");
    }

  memset(cv, 0, sizeof(*cv));
  obj = lv_obj_create(qpk_root_obj());
  if (obj == NULL)
    {
      return JS_ThrowInternalError(ctx, "cannot create canvas");
    }

  qpk_widget_style_plain(obj);
  lv_obj_remove_flag(obj, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_bg_opa(obj, LV_OPA_TRANSP, 0);
  lv_obj_set_pos(obj, x, y);
  lv_obj_set_size(obj, w, h);
  handle = qpk_widget_add(obj, QPK_WIDGET_CANVAS);
  if (handle == 0)
    {
      lv_obj_delete(obj);
      return JS_ThrowInternalError(ctx, "too many widgets");
    }

  cv->obj = obj;
  cv->handle = handle;
  cv->used = true;
  lv_obj_set_user_data(obj, cv);
  lv_obj_add_event_cb(obj, cv_draw, LV_EVENT_DRAW_MAIN, cv);
  lv_obj_add_event_cb(obj, cv_deleted, LV_EVENT_DELETE, cv);

  out = JS_NewObject(ctx);
  JS_SetPropertyStr(ctx, out, "handle", JS_NewInt32(ctx, handle));
  JS_SetPropertyStr(ctx, out, "fillStyle", JS_NewString(ctx, "#000000"));
  JS_SetPropertyStr(ctx, out, "strokeStyle", JS_NewString(ctx, "#000000"));
  JS_SetPropertyStr(ctx, out, "lineWidth", JS_NewInt32(ctx, 1));
  JS_SetPropertyStr(ctx, out, "globalAlpha", JS_NewFloat64(ctx, 1));
  JS_SetPropertyStr(ctx, out, "beginPath",
                    JS_NewCFunction(ctx, js_begin_path, "beginPath", 0));
  JS_SetPropertyStr(ctx, out, "closePath",
                    JS_NewCFunction(ctx, js_close_path, "closePath", 0));
  JS_SetPropertyStr(ctx, out, "moveTo",
                    JS_NewCFunction(ctx, js_move_to, "moveTo", 2));
  JS_SetPropertyStr(ctx, out, "lineTo",
                    JS_NewCFunction(ctx, js_line_to, "lineTo", 2));
  JS_SetPropertyStr(ctx, out, "arc",
                    JS_NewCFunction(ctx, js_arc, "arc", 6));
  JS_SetPropertyStr(ctx, out, "ellipse",
                    JS_NewCFunction(ctx, js_ellipse, "ellipse", 8));
  JS_SetPropertyStr(ctx, out, "quadraticCurveTo",
                    JS_NewCFunction(ctx, js_quad_to, "quadraticCurveTo", 4));
  JS_SetPropertyStr(ctx, out, "bezierCurveTo",
                    JS_NewCFunction(ctx, js_cubic_to, "bezierCurveTo", 6));
  JS_SetPropertyStr(ctx, out, "fill",
                    JS_NewCFunction(ctx, js_fill, "fill", 0));
  JS_SetPropertyStr(ctx, out, "stroke",
                    JS_NewCFunction(ctx, js_stroke, "stroke", 0));
  JS_SetPropertyStr(ctx, out, "fillRect",
                    JS_NewCFunction(ctx, js_fill_rect, "fillRect", 4));
  JS_SetPropertyStr(ctx, out, "strokeRect",
                    JS_NewCFunction(ctx, js_stroke_rect, "strokeRect", 4));
  JS_SetPropertyStr(ctx, out, "clearRect",
                    JS_NewCFunction(ctx, js_clear_rect, "clearRect", 4));
  return out;
}

int qpk_canvas_install(JSContext *ctx, JSValueConst ui)
{
  memset(g_cv, 0, sizeof(g_cv));
  JS_SetPropertyStr(ctx, ui, "canvas",
                    JS_NewCFunction(ctx, js_ui_canvas, "canvas", 4));
  return 0;
}
