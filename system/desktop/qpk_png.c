/****************************************************************************
 * apps/system/desktop/qpk_png.c
 *
 * Deflate + 8-bit non-interlaced RGBA PNG. Allocates with malloc().
 ****************************************************************************/

#include "qpk_png.h"

#include <stdlib.h>
#include <string.h>

#define PNG_OK            0
#define PNG_ERR          -1

struct bit_s
{
  const uint8_t *p;
  const uint8_t *end;
  uint32_t buf;
  int bits;
};

struct huff_s
{
  uint16_t counts[16];
  uint16_t symbols[288];
};

static const uint16_t g_len_base[29] =
{
  3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59,
  67, 83, 99, 115, 131, 163, 195, 227, 258
};

static const uint8_t g_len_extra[29] =
{
  0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4,
  5, 5, 5, 5, 0
};

static const uint16_t g_dist_base[30] =
{
  1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513,
  769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577
};

static const uint8_t g_dist_extra[30] =
{
  0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10,
  11, 11, 12, 12, 13, 13
};

static const uint8_t g_cl_order[19] =
{
  16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15
};

static uint32_t be32(const uint8_t *p)
{
  return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
         ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static int bit_init(struct bit_s *b, const uint8_t *p, size_t n)
{
  b->p = p;
  b->end = p + n;
  b->buf = 0;
  b->bits = 0;
  return 0;
}

static int bit_need(struct bit_s *b, int n)
{
  while (b->bits < n)
    {
      if (b->p >= b->end)
        {
          return PNG_ERR;
        }

      b->buf |= (uint32_t)*b->p++ << b->bits;
      b->bits += 8;
    }

  return PNG_OK;
}

static uint32_t bit_get(struct bit_s *b, int n)
{
  uint32_t v;

  if (n <= 0)
    {
      return 0;
    }

  if (bit_need(b, n) != PNG_OK)
    {
      return 0xffffffffu;
    }

  v = b->buf & ((1u << n) - 1u);
  b->buf >>= n;
  b->bits -= n;
  return v;
}

static int huff_build(struct huff_s *h, const uint8_t *lens, unsigned int n)
{
  uint16_t offs[16];
  unsigned int i;
  unsigned int sum;

  memset(h->counts, 0, sizeof(h->counts));
  for (i = 0; i < n; i++)
    {
      if (lens[i] > 15)
        {
          return PNG_ERR;
        }

      h->counts[lens[i]]++;
    }

  h->counts[0] = 0;
  offs[0] = 0;
  sum = 0;
  for (i = 1; i < 16; i++)
    {
      offs[i] = (uint16_t)sum;
      sum += h->counts[i];
    }

  if (sum > n)
    {
      return PNG_ERR;
    }

  memset(h->symbols, 0, sizeof(h->symbols));
  for (i = 0; i < n; i++)
    {
      if (lens[i] != 0)
        {
          h->symbols[offs[lens[i]]++] = (uint16_t)i;
        }
    }

  return PNG_OK;
}

static int huff_decode(struct bit_s *b, const struct huff_s *h)
{
  int cur = 0;
  int sum = 0;
  int len = 0;

  do
    {
      uint32_t bit = bit_get(b, 1);

      if (bit > 1)
        {
          return PNG_ERR;
        }

      cur = (cur << 1) | (int)bit;
      len++;
      sum += h->counts[len];
      cur -= h->counts[len];
    }
  while (cur >= 0 && len < 15);

  if (cur >= 0)
    {
      return PNG_ERR;
    }

  return h->symbols[sum + cur];
}

static int inflate_block(struct bit_s *b, const struct huff_s *lit,
                         const struct huff_s *dist, uint8_t *out,
                         size_t *pos, size_t cap)
{
  for (;;)
    {
      int sym = huff_decode(b, lit);

      if (sym < 0)
        {
          return PNG_ERR;
        }

      if (sym < 256)
        {
          if (*pos >= cap)
            {
              return PNG_ERR;
            }

          out[(*pos)++] = (uint8_t)sym;
        }
      else if (sym == 256)
        {
          return PNG_OK;
        }
      else
        {
          int leni = sym - 257;
          uint32_t length;
          uint32_t extra;
          int dist_sym;
          uint32_t distance;
          uint32_t k;

          if (leni < 0 || leni > 28)
            {
              return PNG_ERR;
            }

          extra = bit_get(b, g_len_extra[leni]);
          if (g_len_extra[leni] && extra == 0xffffffffu)
            {
              return PNG_ERR;
            }

          length = g_len_base[leni] + extra;
          dist_sym = huff_decode(b, dist);
          if (dist_sym < 0 || dist_sym > 29)
            {
              return PNG_ERR;
            }

          extra = bit_get(b, g_dist_extra[dist_sym]);
          if (g_dist_extra[dist_sym] && extra == 0xffffffffu)
            {
              return PNG_ERR;
            }

          distance = g_dist_base[dist_sym] + extra;
          if (distance == 0 || distance > *pos || *pos + length > cap)
            {
              return PNG_ERR;
            }

          for (k = 0; k < length; k++)
            {
              out[*pos] = out[*pos - distance];
              (*pos)++;
            }
        }
    }
}

static int inflate_dynamic(struct bit_s *b, uint8_t *out, size_t *pos,
                           size_t cap)
{
  struct huff_s cl;
  struct huff_s lit;
  struct huff_s dist;
  uint8_t clens[19];
  uint8_t lens[288 + 32];
  unsigned int nlit;
  unsigned int ndist;
  unsigned int nclen;
  unsigned int i;
  unsigned int n;

  nlit = 257 + bit_get(b, 5);
  ndist = 1 + bit_get(b, 5);
  nclen = 4 + bit_get(b, 4);
  if (nlit > 288 || ndist > 32 || nclen > 19)
    {
      return PNG_ERR;
    }

  memset(clens, 0, sizeof(clens));
  for (i = 0; i < nclen; i++)
    {
      clens[g_cl_order[i]] = (uint8_t)bit_get(b, 3);
    }

  if (huff_build(&cl, clens, 19) != PNG_OK)
    {
      return PNG_ERR;
    }

  n = nlit + ndist;
  i = 0;
  while (i < n)
    {
      int s = huff_decode(b, &cl);
      unsigned int rep;
      uint8_t v;

      if (s < 0)
        {
          return PNG_ERR;
        }

      if (s < 16)
        {
          lens[i++] = (uint8_t)s;
          continue;
        }

      if (s == 16)
        {
          if (i == 0)
            {
              return PNG_ERR;
            }

          v = lens[i - 1];
          rep = 3 + bit_get(b, 2);
        }
      else if (s == 17)
        {
          v = 0;
          rep = 3 + bit_get(b, 3);
        }
      else if (s == 18)
        {
          v = 0;
          rep = 11 + bit_get(b, 7);
        }
      else
        {
          return PNG_ERR;
        }

      if (i + rep > n)
        {
          return PNG_ERR;
        }

      while (rep--)
        {
          lens[i++] = v;
        }
    }

  if (huff_build(&lit, lens, nlit) != PNG_OK ||
      huff_build(&dist, lens + nlit, ndist) != PNG_OK)
    {
      return PNG_ERR;
    }

  return inflate_block(b, &lit, &dist, out, pos, cap);
}

static int inflate_fixed(struct bit_s *b, uint8_t *out, size_t *pos,
                         size_t cap)
{
  struct huff_s lit;
  struct huff_s dist;
  uint8_t lens[288];
  uint8_t dlens[32];
  int i;

  for (i = 0; i < 144; i++)
    {
      lens[i] = 8;
    }

  for (; i < 256; i++)
    {
      lens[i] = 9;
    }

  for (; i < 280; i++)
    {
      lens[i] = 7;
    }

  for (; i < 288; i++)
    {
      lens[i] = 8;
    }

  for (i = 0; i < 32; i++)
    {
      dlens[i] = 5;
    }

  if (huff_build(&lit, lens, 288) != PNG_OK ||
      huff_build(&dist, dlens, 32) != PNG_OK)
    {
      return PNG_ERR;
    }

  return inflate_block(b, &lit, &dist, out, pos, cap);
}

static int zlib_inflate(const uint8_t *src, size_t srclen, uint8_t *out,
                        size_t cap, size_t *outlen)
{
  struct bit_s b;
  size_t pos = 0;
  int last;

  if (srclen < 2)
    {
      return PNG_ERR;
    }

  /* zlib CMF/FLG; reject preset dictionary */
  if ((src[0] & 0x0f) != 8 || ((src[0] << 8) + src[1]) % 31 != 0 ||
      (src[1] & 0x20) != 0)
    {
      return PNG_ERR;
    }

  bit_init(&b, src + 2, srclen - 2);
  do
    {
      int type;

      last = (int)bit_get(&b, 1);
      type = (int)bit_get(&b, 2);
      if (type == 0)
        {
          unsigned int len;
          unsigned int nlen;

          b.buf = 0;
          b.bits = 0;
          if (b.p + 4 > b.end)
            {
              return PNG_ERR;
            }

          len = (unsigned int)b.p[0] | ((unsigned int)b.p[1] << 8);
          nlen = (unsigned int)b.p[2] | ((unsigned int)b.p[3] << 8);
          b.p += 4;
          if ((len ^ 0xffffu) != nlen || b.p + len > b.end ||
              pos + len > cap)
            {
              return PNG_ERR;
            }

          memcpy(out + pos, b.p, len);
          b.p += len;
          pos += len;
        }
      else if (type == 1)
        {
          if (inflate_fixed(&b, out, &pos, cap) != PNG_OK)
            {
              return PNG_ERR;
            }
        }
      else if (type == 2)
        {
          if (inflate_dynamic(&b, out, &pos, cap) != PNG_OK)
            {
              return PNG_ERR;
            }
        }
      else
        {
          return PNG_ERR;
        }
    }
  while (!last);

  *outlen = pos;
  return PNG_OK;
}

static uint8_t paeth(int a, int b, int c)
{
  int p = a + b - c;
  int pa = p > a ? p - a : a - p;
  int pb = p > b ? p - b : b - p;
  int pc = p > c ? p - c : c - p;

  if (pa <= pb && pa <= pc)
    {
      return (uint8_t)a;
    }

  if (pb <= pc)
    {
      return (uint8_t)b;
    }

  return (uint8_t)c;
}

static int unfilter(uint8_t *out, const uint8_t *in, unsigned int w,
                    unsigned int h)
{
  unsigned int stride = w * 4;
  unsigned int y;
  unsigned int x;

  for (y = 0; y < h; y++)
    {
      int type = *in++;
      uint8_t *dst = out + (size_t)y * stride;
      const uint8_t *prev = y ? out + (size_t)(y - 1) * stride : NULL;

      switch (type)
        {
          case 0:
            memcpy(dst, in, stride);
            break;

          case 1:
            for (x = 0; x < stride; x++)
              {
                dst[x] = (uint8_t)(in[x] + (x >= 4 ? dst[x - 4] : 0));
              }
            break;

          case 2:
            for (x = 0; x < stride; x++)
              {
                dst[x] = (uint8_t)(in[x] + (prev ? prev[x] : 0));
              }
            break;

          case 3:
            for (x = 0; x < stride; x++)
              {
                unsigned int a = x >= 4 ? dst[x - 4] : 0;
                unsigned int b = prev ? prev[x] : 0;

                dst[x] = (uint8_t)(in[x] + (a + b) / 2);
              }
            break;

          case 4:
            for (x = 0; x < stride; x++)
              {
                int a = x >= 4 ? dst[x - 4] : 0;
                int b = prev ? prev[x] : 0;
                int c = (prev && x >= 4) ? prev[x - 4] : 0;

                dst[x] = (uint8_t)(in[x] + paeth(a, b, c));
              }
            break;

          default:
            return PNG_ERR;
        }

      in += stride;
    }

  return PNG_OK;
}

int qpk_png_decode32(uint8_t **out, unsigned int *width, unsigned int *height,
                     const uint8_t *png, size_t size)
{
  static const uint8_t sig[8] =
    {
      0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a
    };
  const uint8_t *p;
  const uint8_t *end;
  uint8_t *idat = NULL;
  size_t idat_len = 0;
  uint8_t *scan = NULL;
  uint8_t *rgba = NULL;
  unsigned int w = 0;
  unsigned int h = 0;
  unsigned int bit = 0;
  unsigned int color = 0;
  unsigned int interlace = 0;
  int have_ihdr = 0;
  size_t expected;
  size_t inflated = 0;
  int ret = PNG_ERR;

  if (out == NULL || width == NULL || height == NULL || png == NULL ||
      size < 33)
    {
      return PNG_ERR;
    }

  *out = NULL;
  *width = 0;
  *height = 0;
  if (memcmp(png, sig, 8) != 0)
    {
      return PNG_ERR;
    }

  p = png + 8;
  end = png + size;
  while (p + 12 <= end)
    {
      uint32_t len = be32(p);
      const uint8_t *type = p + 4;
      const uint8_t *data = p + 8;

      if (len > (uint32_t)(end - data - 4))
        {
          goto done;
        }

      if (memcmp(type, "IHDR", 4) == 0)
        {
          if (len != 13)
            {
              goto done;
            }

          w = be32(data);
          h = be32(data + 4);
          bit = data[8];
          color = data[9];
          interlace = data[12];
          have_ihdr = 1;
        }
      else if (memcmp(type, "IDAT", 4) == 0)
        {
          uint8_t *nbuf = realloc(idat, idat_len + len);

          if (nbuf == NULL)
            {
              goto done;
            }

          idat = nbuf;
          memcpy(idat + idat_len, data, len);
          idat_len += len;
        }
      else if (memcmp(type, "IEND", 4) == 0)
        {
          break;
        }

      p = data + len + 4;
    }

  if (!have_ihdr || idat == NULL || w == 0 || h == 0 || w > 2048 ||
      h > 2048 || bit != 8 || color != 6 || interlace != 0)
    {
      goto done;
    }

  expected = (size_t)h * (1u + (size_t)w * 4u);
  scan = malloc(expected);
  rgba = malloc((size_t)w * h * 4u);
  if (scan == NULL || rgba == NULL)
    {
      goto done;
    }

  if (zlib_inflate(idat, idat_len, scan, expected, &inflated) != PNG_OK ||
      inflated != expected)
    {
      goto done;
    }

  if (unfilter(rgba, scan, w, h) != PNG_OK)
    {
      goto done;
    }

  *out = rgba;
  *width = w;
  *height = h;
  rgba = NULL;
  ret = PNG_OK;

done:
  free(idat);
  free(scan);
  free(rgba);
  return ret;
}
