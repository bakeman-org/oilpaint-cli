/* oilpaint.c — single-file oil-paint CLI

 构建：
   stb 后端（零依赖）:
     cc -O2 -o oilpaint_stb oilpaint.c -lm
   libvips 后端:
     cc -O2 -DHAVE_VIPS -o oilpaint_vips oilpaint.c $(pkg-config --cflags --libs vips) -lm

 */

#include <errno.h>
#include <getopt.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#ifdef HAVE_VIPS
#include <vips/vips.h>
#else
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"
#define STB_IMAGE_RESIZE_IMPLEMENTATION
#include "stb_image_resize2.h"
#endif

/* ============================================================
 * 滤镜部分（与原 JS 实现一一对应）
 * ============================================================ */

#define REFERENCE_EDGE 400

/* 饱和度调整：向亮度灰度值做线性插值 */
static void apply_saturation(uint8_t *pixels, size_t npix, float saturation) {
  if (!isfinite(saturation) || saturation == 1.0f)
    return;
  for (size_t i = 0; i < npix; i++) {
    uint8_t *p = pixels + i * 3;
    float r = p[0], g = p[1], b = p[2];
    float luma = 0.299f * r + 0.587f * g + 0.114f * b;
    p[0] = (uint8_t)fminf(255, fmaxf(0, luma + (r - luma) * saturation) + 0.5f);
    p[1] = (uint8_t)fminf(255, fmaxf(0, luma + (g - luma) * saturation) + 0.5f);
    p[2] = (uint8_t)fminf(255, fmaxf(0, luma + (b - luma) * saturation) + 0.5f);
  }
}

/* 32 位整数哈希，输出 [0,1) 浮点。等价于 JS 里的 Math.imul 版本 */
static inline float hash2(int x, int y) {
  uint32_t h = (uint32_t)x * 374761393u + (uint32_t)y * 668265263u;
  h = (h ^ (h >> 13)) * 1274126177u;
  h ^= h >> 16;
  return h / 4294967296.0f;
}

/* 双线性插值的值噪声（smoothstep 过渡） */
static float smooth_noise(int x, int y, float scale) {
  float fx = x / scale, fy = y / scale;
  int ix = (int)floorf(fx), iy = (int)floorf(fy);
  float tx = fx - ix, ty = fy - iy;
  float sx = tx * tx * (3 - 2 * tx);
  float sy = ty * ty * (3 - 2 * ty);
  float n00 = hash2(ix, iy), n10 = hash2(ix + 1, iy);
  float n01 = hash2(ix, iy + 1), n11 = hash2(ix + 1, iy + 1);
  float top = n00 + (n10 - n00) * sx;
  float bottom = n01 + (n11 - n01) * sx;
  return top + (bottom - top) * sy;
}

/* 在像素上叠加画布编织纹 + 细颗粒 + 大尺度颜料不均 */
static void apply_canvas_texture(uint8_t *pixels, int width, int height,
                                 float strength) {
  int long_edge = width > height ? width : height;
  float amplitude = strength * 30.0f;
  /* 编织和大块斑驳的周期随图像尺寸缩放，避免高分辨率时被平均掉 */
  float weave_period = fmaxf(6.0f, long_edge / 90.0f);
  float weave_freq = (float)(M_PI * 2.0 / weave_period);
  float broad_scale = fmaxf(8.0f, long_edge / 45.0f);

  for (int y = 0; y < height; y++) {
    for (int x = 0; x < width; x++) {
      /* 斜向亚麻编织 */
      float weave = sinf(x * weave_freq + y * weave_freq * 0.15f) *
                    sinf(y * weave_freq - x * weave_freq * 0.15f);
      float grain = hash2(x, y) - 0.5f;
      float broad = smooth_noise(x, y, broad_scale) - 0.5f;
      float delta = amplitude * (weave * 0.3f + grain * 0.35f + broad * 0.5f);
      uint8_t *p = pixels + (y * width + x) * 3;
      for (int c = 0; c < 3; c++) {
        float v = p[c] + delta;
        p[c] = (uint8_t)fminf(255, fmaxf(0, v + 0.5f));
      }
    }
  }
}

/* Kuwahara 滤镜：用积分图（summed-area table）加速四象限均值和方差计算。
 * 每个像素在 4 个重叠象限里挑“方差最小”的那个，取其均值作为输出，
 * 这就是油画笔触的核心来源：平坦区域被抹平为色块，边缘处因方差大被保留。 */
static uint8_t *kuwahara(const uint8_t *src, int width, int height,
                         int radius) {
  int stride = width + 1;
  size_t plane = (size_t)stride * (height + 1);

  /* 6 张积分表：R/G/B 的求和 与 平方和。
   * 用 double 防止 1600x1600 图像下平方和溢出（255^2 * 2.5M ≈ 1.6e11 > 2^32）。 */
  double *sumR = calloc(plane, sizeof(double));
  double *sumG = calloc(plane, sizeof(double));
  double *sumB = calloc(plane, sizeof(double));
  double *sqR = calloc(plane, sizeof(double));
  double *sqG = calloc(plane, sizeof(double));
  double *sqB = calloc(plane, sizeof(double));
  if (!sumR || !sumG || !sumB || !sqR || !sqG || !sqB) {
    fprintf(stderr, "oilpaint: out of memory in kuwahara SAT\n");
    exit(1);
  }

  /* 第一遍：逐行累加构造积分图 */
  for (int y = 0; y < height; y++) {
    double rowR = 0, rowG = 0, rowB = 0;
    double rowSqR = 0, rowSqG = 0, rowSqB = 0;
    const uint8_t *srcRow = src + (size_t)y * width * 3;
    int cur = (y + 1) * stride;
    int prev = y * stride;

    for (int x = 0; x < width; x++) {
      const uint8_t *p = srcRow + (size_t)x * 3;
      double r = p[0], g = p[1], b = p[2];
      rowR += r;
      rowG += g;
      rowB += b;
      rowSqR += r * r;
      rowSqG += g * g;
      rowSqB += b * b;

      int i = cur + x + 1;
      int up = prev + x + 1;
      sumR[i] = sumR[up] + rowR;
      sumG[i] = sumG[up] + rowG;
      sumB[i] = sumB[up] + rowB;
      sqR[i] = sqR[up] + rowSqR;
      sqG[i] = sqG[up] + rowSqG;
      sqB[i] = sqB[up] + rowSqB;
    }
  }

  uint8_t *out = malloc((size_t)width * height * 3);
  if (!out) {
    fprintf(stderr, "oilpaint: out of memory in kuwahara out\n");
    exit(1);
  }

  /* 四象限坐标缓存，避免每像素分配数组 */
  int qx0[4], qx1[4], qy0[4], qy1[4];

  /* 第二遍：逐像素挑方差最小的象限 */
  for (int y = 0; y < height; y++) {
    int top0 = y - radius < 0 ? 0 : y - radius;
    int top1 = y + 1;
    int bottom0 = y;
    int bottom1 = y + radius + 1 > height ? height : y + radius + 1;

    for (int x = 0; x < width; x++) {
      int left0 = x - radius < 0 ? 0 : x - radius;
      int left1 = x + 1;
      int right0 = x;
      int right1 = x + radius + 1 > width ? width : x + radius + 1;

      /* 左上、右上、左下、右下 */
      qx0[0] = left0;   qx1[0] = left1;  qy0[0] = top0;    qy1[0] = top1;
      qx0[1] = right0;  qx1[1] = right1; qy0[1] = top0;    qy1[1] = top1;
      qx0[2] = left0;   qx1[2] = left1;  qy0[2] = bottom0; qy1[2] = bottom1;
      qx0[3] = right0;  qx1[3] = right1; qy0[3] = bottom0; qy1[3] = bottom1;

      double bestVar = INFINITY, bestR = 0, bestG = 0, bestB = 0;

      for (int q = 0; q < 4; q++) {
        int x0 = qx0[q], x1 = qx1[q], y0 = qy0[q], y1 = qy1[q];
        int count = (x1 - x0) * (y1 - y0);
        if (count == 0)
          continue;

        /* 积分图上的四点查表，O(1) 得到矩形区域的求和 */
        int a = y0 * stride + x0, b = y0 * stride + x1;
        int c = y1 * stride + x0, d = y1 * stride + x1;

        double meanR = (sumR[d] - sumR[b] - sumR[c] + sumR[a]) / count;
        double meanG = (sumG[d] - sumG[b] - sumG[c] + sumG[a]) / count;
        double meanB = (sumB[d] - sumB[b] - sumB[c] + sumB[a]) / count;

        /* Var = E[X^2] - E[X]^2，clamp 到 0 防止浮点误差产生负方差 */
        double varR = fmax(0, (sqR[d] - sqR[b] - sqR[c] + sqR[a]) / count - meanR * meanR);
        double varG = fmax(0, (sqG[d] - sqG[b] - sqG[c] + sqG[a]) / count - meanG * meanG);
        double varB = fmax(0, (sqB[d] - sqB[b] - sqB[c] + sqB[a]) / count - meanB * meanB);

        double variance = varR + varG + varB;
        if (variance < bestVar) {
          bestVar = variance;
          bestR = meanR;
          bestG = meanG;
          bestB = meanB;
        }
      }

      size_t o = ((size_t)y * width + x) * 3;
      out[o]     = (uint8_t)(bestR + 0.5);
      out[o + 1] = (uint8_t)(bestG + 0.5);
      out[o + 2] = (uint8_t)(bestB + 0.5);
    }
  }

  free(sumR); free(sumG); free(sumB);
  free(sqR);  free(sqG);  free(sqB);
  return out;
}

/* 完整滤镜管线：Kuwahara → 饱和度 → 画布纹理 */
static uint8_t *oil_paint_pixels(const uint8_t *rgb, int width, int height,
                                 int radius, float texture, float saturation) {
  int long_edge = width > height ? width : height;
  /* 笔刷半径按 REFERENCE_EDGE 缩放，保证不同分辨率下视觉一致 */
  int brush = (int)roundf((float)radius * long_edge / REFERENCE_EDGE);
  if (brush < 1)
    brush = 1;

  uint8_t *painted = kuwahara(rgb, width, height, brush);
  apply_saturation(painted, (size_t)width * height, saturation);
  if (texture > 0)
    apply_canvas_texture(painted, width, height, texture);
  return painted;
}

/* ============================================================
 * 图像后端
 * ============================================================ */

typedef struct {
  int width, height;
  uint8_t *rgb;  /* packed RGB, 3 字节/像素 */
} Image;

#ifdef HAVE_VIPS

/* 使用 libvips 加载：autorot（EXIF）→ flatten（透明压白）→ sRGB → lanczos3 缩放 → 取 3 通道 */
static Image load_image_vips(const char *path, int max_edge) {
  Image img = {0, 0, NULL};
  VipsImage *in = NULL, *rotated = NULL, *srgb = NULL, *resized = NULL,
            *rgb = NULL, *mem = NULL;

  if (!(in = vips_image_new_from_file(path, "fail-on", VIPS_FAIL_ON_NONE,
                                      NULL))) {
    fprintf(stderr, "oilpaint: cannot open %s: %s\n", path, vips_error_buffer());
    return img;
  }

  /* 处理 EXIF 方向 */
  if (vips_autorot(in, &rotated, NULL))
    goto fail;
  g_object_unref(in);
  in = NULL;

  /* 有 alpha 通道就压到白底，等价于 sharp().flatten({background:'#ffffff'}) */
  if (vips_image_hasalpha(rotated)) {
    VipsImage *flat = NULL;
    double bg[3] = {255.0, 255.0, 255.0};
    VipsArrayDouble *background = vips_array_double_new(bg, 3);
    int r = vips_flatten(rotated, &flat, "background", background,
                         "max_alpha", 255.0, NULL);
    vips_area_unref(VIPS_AREA(background));
    if (r)
      goto fail;
    g_object_unref(rotated);
    rotated = flat;
  }

  /* 色彩空间统一到 sRGB */
  if (vips_colourspace(rotated, &srgb, VIPS_INTERPRETATION_sRGB, NULL))
    goto fail;
  g_object_unref(rotated);
  rotated = NULL;

  /* 只在需要时缩小，withEnlargement=false */
  int w = vips_image_get_width(srgb);
  int h = vips_image_get_height(srgb);
  double scale = 1.0;
  if (w > max_edge || h > max_edge)
    scale = (double)max_edge / (w > h ? w : h);

  if (scale < 1.0) {
    if (vips_resize(srgb, &resized, scale, "kernel", VIPS_KERNEL_LANCZOS3, NULL))
      goto fail;
    g_object_unref(srgb);
    srgb = NULL;
  } else {
    resized = g_object_ref(srgb);
    g_object_unref(srgb);
    srgb = NULL;
  }

  /* 丢弃多余通道，只保留 RGB */
  if (vips_extract_band(resized, &rgb, 0, "n", 3, NULL))
    goto fail;
  g_object_unref(resized);
  resized = NULL;

  /* 复制到连续内存，防止懒加载指针悬空 */
  if (!(mem = vips_image_copy_memory(rgb)))
    goto fail;
  g_object_unref(rgb);
  rgb = NULL;

  img.width = vips_image_get_width(mem);
  img.height = vips_image_get_height(mem);
  size_t sz = (size_t)img.width * img.height * 3;
  img.rgb = malloc(sz);
  if (!img.rgb) {
    fprintf(stderr, "oilpaint: out of memory\n");
    goto fail;
  }
  memcpy(img.rgb, VIPS_IMAGE_ADDR(mem, 0, 0), sz);

  g_object_unref(mem);
  return img;

fail:
  fprintf(stderr, "oilpaint: libvips error: %s\n", vips_error_buffer());
  if (in)      g_object_unref(in);
  if (rotated) g_object_unref(rotated);
  if (srgb)    g_object_unref(srgb);
  if (resized) g_object_unref(resized);
  if (rgb)     g_object_unref(rgb);
  if (mem)     g_object_unref(mem);
  return img;
}

/* 用 libvips 保存。返回 0 成功 / -1 失败。
 *
 * 注意 vips_copy 的签名是 int vips_copy(VipsImage *in, VipsImage **out, ...)：
 * 它不返回指针，而是通过第二个参数 &out 输出新图像，返回 int 表示成功与否。 */
static int save_image_vips(const Image *img, const char *path,
                           const char *format, int quality) {
  size_t sz = (size_t)img->width * img->height * 3;

  /* vips_image_new_from_memory 只是包装外部 buffer，不做拷贝 */
  VipsImage *base = vips_image_new_from_memory(
      img->rgb, sz, img->width, img->height, 3, VIPS_FORMAT_UCHAR);
  if (!base) {
    fprintf(stderr, "oilpaint: cannot wrap memory into vips image\n");
    return -1;
  }

  /* 显式标记 interpretation 为 sRGB，确保编码器正确识别色彩空间 */
  VipsImage *out = NULL;
  if (vips_copy(base, &out, "interpretation", VIPS_INTERPRETATION_sRGB, NULL)) {
    fprintf(stderr, "oilpaint: vips_copy failed: %s\n", vips_error_buffer());
    g_object_unref(base);
    return -1;
  }
  g_object_unref(base);

  int ret;
  if (strcmp(format, "jpeg") == 0)
    ret = vips_jpegsave(out, path, "Q", quality, NULL);
  else if (strcmp(format, "webp") == 0)
    ret = vips_webpsave(out, path, "Q", quality, NULL);
  else
    ret = vips_pngsave(out, path, "compression", 9, NULL);

  if (ret)
    fprintf(stderr, "oilpaint: vips save failed (%s): %s\n", format,
            vips_error_buffer());

  g_object_unref(out);
  /* vips_*save 返回 0 成功、-1 失败；这里显式归一化 */
  return ret ? -1 : 0;
}

#else /* STB 后端 */

/* 使用 stb 加载：自动缩放到 max_edge 以内 */
static Image load_image_stb(const char *path, int max_edge) {
  Image img = {0, 0, NULL};
  int w, h, ch;

  /* 强制解码成 3 通道，丢弃 alpha */
  stbi_uc *data = stbi_load(path, &w, &h, &ch, 3);
  if (!data) {
    fprintf(stderr, "oilpaint: cannot open %s: %s\n", path, stbi_failure_reason());
    return img;
  }

  if (w > max_edge || h > max_edge) {
    float scale = (float)max_edge / (w > h ? w : h);
    int nw = (int)(w * scale + 0.5f);
    int nh = (int)(h * scale + 0.5f);
    if (nw < 1) nw = 1;
    if (nh < 1) nh = 1;

    stbi_uc *resized = malloc((size_t)nw * nh * 3);
    if (!resized) {
      stbi_image_free(data);
      return img;
    }
    stbir_resize_uint8_srgb(data, w, h, 0, resized, nw, nh, 0, STBIR_RGB);
    stbi_image_free(data);
    data = resized;
    w = nw;
    h = nh;
  }

  img.width = w;
  img.height = h;
  img.rgb = data;
  return img;
}

/* 用 stb 保存。返回 0 成功 / -1 失败。
 * 注意：stbi_write_* 的语义是“非零成功、零失败”，与 libvips 相反，
 * 这里统一转换成 0/-1，避免上层判断逻辑分叉。 */
static int save_image_stb(const Image *img, const char *path,
                          const char *format, int quality) {
  int ok;

  if (strcmp(format, "jpeg") == 0) {
    ok = stbi_write_jpg(path, img->width, img->height, 3, img->rgb, quality);
  } else {
    if (strcmp(format, "webp") == 0)
      fprintf(stderr,
              "oilpaint: WebP not supported in stb backend, writing PNG instead\n");
    ok = stbi_write_png(path, img->width, img->height, 3, img->rgb,
                        img->width * 3);
  }

  if (!ok)
    fprintf(stderr, "oilpaint: stb save failed (%s): %s\n", format, path);

  return ok ? 0 : -1;
}

#endif /* HAVE_VIPS */

/* 后端分发的统一入口 */
static Image load_image(const char *path, int max_edge) {
#ifdef HAVE_VIPS
  return load_image_vips(path, max_edge);
#else
  return load_image_stb(path, max_edge);
#endif
}

/* 统一返回 0 成功 / -1 失败 */
static int save_image(const Image *img, const char *path, const char *format,
                      int quality) {
#ifdef HAVE_VIPS
  return save_image_vips(img, path, format, quality);
#else
  return save_image_stb(img, path, format, quality);
#endif
}

/* ============================================================
 * 工具函数
 * ============================================================ */

/* 递归创建目录，等价于 mkdir -p。
 * 用 libc 的 mkdir 而不是 system("mkdir -p ...")，避免 shell 注入以及
 * 对路径中单引号的转义问题。 */
static int mkdir_p(const char *path) {
  char tmp[2048];
  size_t len = strlen(path);
  if (len == 0 || len >= sizeof(tmp))
    return -1;
  memcpy(tmp, path, len + 1);
  if (tmp[len - 1] == '/')
    tmp[len - 1] = '\0';

  /* 从第 2 个字符开始找 '/'，逐段创建 */
  for (char *p = tmp + 1; *p; p++) {
    if (*p == '/') {
      *p = '\0';
      if (mkdir(tmp, 0755) != 0 && errno != EEXIST)
        return -1;
      *p = '/';
    }
  }
  if (mkdir(tmp, 0755) != 0 && errno != EEXIST)
    return -1;
  return 0;
}

/* 把用户输入的格式规范化成内部使用的名字：
 *   jpg / JPG / jpeg / JPEG → jpeg
 *   png / PNG               → png
 *   webp / WEBP             → webp
 * 其它情况返回 NULL。 */
static const char *normalize_format(const char *fmt) {
  if (!fmt)
    return NULL;
  if (strcasecmp(fmt, "png") == 0)  return "png";
  if (strcasecmp(fmt, "jpg") == 0)  return "jpeg";
  if (strcasecmp(fmt, "jpeg") == 0) return "jpeg";
  if (strcasecmp(fmt, "webp") == 0) return "webp";
  return NULL;
}

/* 从路径扩展名推断格式，无扩展名默认 png */
static const char *format_from_path(const char *path) {
  const char *dot = strrchr(path, '.');
  if (!dot)
    return "png";
  const char *norm = normalize_format(dot + 1);
  return norm ? norm : "png";
}

/* 输出文件名用的扩展名：jpeg → jpg，其它保持原样 */
static const char *out_ext(const char *format) {
  return strcmp(format, "jpeg") == 0 ? "jpg" : format;
}

/* ============================================================
 * CLI
 * ============================================================ */

static void usage(const char *prog) {
  fprintf(
      stderr,
      "oilpaint - turn photos into oil paintings\n\n"
      "Usage:\n  %s [options] <image...>\n\n"
      "Options:\n"
      "  -o, --out <path>        Output file or directory (default: ./oilpaint-output)\n"
      "  -r, --radius <n>        Brush size, 1-24 (default: 4)\n"
      "  -m, --max <px>          Max long edge, 64+ (default: 1600)\n"
      "      --texture <0-1>     Canvas texture strength (default: 0.35)\n"
      "  -s, --saturation <n>    Saturation multiplier, 0-3 (default: 1)\n"
      "  -f, --format <fmt>      png, jpeg or webp (default: from extension)\n"
      "  -q, --quality <1-100>   JPEG/WebP quality (default: 92)\n"
      "  -h, --help              Show this help\n",
      prog);
}

int main(int argc, char **argv) {
#ifdef HAVE_VIPS
  if (VIPS_INIT(argv[0]))
    vips_error_exit(NULL);
#endif

  const char *out_path = NULL;
  int radius = 4, max_edge = 1600, quality = 92;
  float texture = 0.35f, saturation = 1.0f;
  const char *format_opt = NULL;

  static struct option long_opts[] = {
      {"out",        required_argument, 0, 'o'},
      {"radius",     required_argument, 0, 'r'},
      {"max",        required_argument, 0, 'm'},
      {"texture",    required_argument, 0, 1000},
      {"saturation", required_argument, 0, 's'},
      {"format",     required_argument, 0, 'f'},
      {"quality",    required_argument, 0, 'q'},
      {"help",       no_argument,       0, 'h'},
      {0, 0, 0, 0}};

  int c;
  while ((c = getopt_long(argc, argv, "o:r:m:s:f:q:h", long_opts, NULL)) != -1) {
    switch (c) {
    case 'o': out_path = optarg; break;
    case 'r': radius = atoi(optarg); break;
    case 'm': max_edge = atoi(optarg); break;
    case 1000: texture = atof(optarg); break;
    case 's': saturation = atof(optarg); break;
    case 'f': format_opt = optarg; break;
    case 'q': quality = atoi(optarg); break;
    case 'h': usage(argv[0]); return 0;
    default: usage(argv[0]); return 2;
    }
  }

  if (optind >= argc) {
    usage(argv[0]);
    return 2;
  }

  /* 校验 --format 参数 */
  if (format_opt && !normalize_format(format_opt)) {
    fprintf(stderr, "oilpaint: unsupported format \"%s\" (use png, jpeg or webp)\n",
            format_opt);
    return 2;
  }

  /* 判断输出模式：
   *   - 多输入必然是多文件（视为目录）
   *   - 单输入且 out_path 有扩展名 → 视为具体文件名
   *   - 单输入且 out_path 是目录（无扩展名或有斜杠）→ 目录模式 */
  int multi = (argc - optind) > 1;
  char out_dir[1024] = "oilpaint-output";
  if (out_path && !multi) {
    const char *dot = strrchr(out_path, '.');
    if (!(dot && strchr(dot, '/') == NULL)) {
      /* 没有扩展名或扩展名前面有路径分隔符 → 当作目录 */
      snprintf(out_dir, sizeof(out_dir), "%s", out_path);
      out_path = NULL;
    }
  } else if (out_path) {
    snprintf(out_dir, sizeof(out_dir), "%s", out_path);
  }

  int failures = 0;

  for (int i = optind; i < argc; i++) {
    const char *input = argv[i];

    /* 决定输出格式：显式参数 > 输出文件扩展名 > 输入扩展名 > png */
    const char *format;
    if (format_opt) {
      format = normalize_format(format_opt);
    } else if (out_path && !multi) {
      format = format_from_path(out_path);
    } else {
      format = format_from_path(input);
    }

    printf("loading %s ...\n", input);
    Image img = load_image(input, max_edge);
    if (!img.rgb) {
      failures++;
      continue;
    }

    printf("  %dx%d, painting (radius=%d, texture=%.2f) ...\n", img.width,
           img.height, radius, texture);

    uint8_t *painted = oil_paint_pixels(img.rgb, img.width, img.height, radius,
                                        texture, saturation);
    free(img.rgb);
    img.rgb = painted;

    /* 拼输出路径 */
    char out_file[2048];
    if (out_path && !multi) {
      /* 单文件模式：直接用用户指定的文件名，若缺扩展名则补齐 */
      const char *dot = strrchr(out_path, '.');
      if (dot && strchr(dot, '/') == NULL) {
        snprintf(out_file, sizeof(out_file), "%s", out_path);
      } else {
        snprintf(out_file, sizeof(out_file), "%s.%s", out_path, out_ext(format));
      }
    } else {
      /* 目录模式：取输入文件的 basename 做 stem，加 -oilpaint 后缀 */
      const char *base = strrchr(input, '/');
      base = base ? base + 1 : input;
      char stem[512];
      snprintf(stem, sizeof(stem), "%s", base);
      char *dot = strrchr(stem, '.');
      if (dot)
        *dot = '\0';
      snprintf(out_file, sizeof(out_file), "%s/%s-oilpaint.%s", out_dir, stem,
               out_ext(format));
    }

    printf("  -> %s\n", out_file);

    /* 若路径含目录部分，先递归创建 */
    char *slash = strrchr(out_file, '/');
    if (slash) {
      *slash = '\0';
      if (mkdir_p(out_file) != 0)
        fprintf(stderr, "oilpaint: cannot create directory %s\n", out_file);
      *slash = '/';
    }

    /* 两个后端的 save_image 都返回 0 成功 / -1 失败 */
    int save_code = save_image(&img, out_file, format, quality);
    if (save_code != 0) {
      fprintf(stderr, "oilpaint: failed to save %s, save return code: %d\n",
              out_file, save_code);
      failures++;
    }

    free(img.rgb);
  }

#ifdef HAVE_VIPS
  vips_shutdown();
#endif

  return failures > 0 ? 1 : 0;
}