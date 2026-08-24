/****************************************************************************
 * apps/interpreters/quickjs/qjs_nuttx_compat.c
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <limits.h>
#include <math.h>

/* Bellard QuickJS expects a small C99 libm surface that is not supplied by
 * NuttX's compact built-in libm.  Keep these shims local to the QuickJS
 * archive so enabling JavaScript does not replace the board's proven math
 * and float-header configuration.
 */

int fesetround(int mode)
{
#if defined(CONFIG_ARCH_RISCV) && defined(CONFIG_ARCH_FPU)
  unsigned int frm;

  switch (mode)
    {
      case 0: /* FE_TONEAREST */
        frm = 0;
        break;
      case 1: /* FE_TOWARDZERO */
        frm = 1;
        break;
      case 2: /* FE_DOWNWARD */
        frm = 2;
        break;
      case 3: /* FE_UPWARD */
        frm = 3;
        break;
      default:
        errno = EINVAL;
        return -1;
    }

  __asm__ __volatile__("fsrm %0" : : "r"(frm));
  return 0;
#else
  /* This board's stable configuration uses compiler soft-float support.
   * There is no hardware rounding CSR for C arithmetic in that mode, so
   * accept the four C99 modes while retaining libgcc's default rounding.
   */

  if (mode < 0 || mode > 3)
    {
      errno = EINVAL;
      return -1;
    }

  return 0;
#endif
}

long lrint(double value)
{
  double rounded = rint(value);

  if (rounded >= (double)LONG_MAX)
    {
      return LONG_MAX;
    }

  if (rounded <= (double)LONG_MIN)
    {
      return LONG_MIN;
    }

  return (long)rounded;
}

double hypot(double x, double y)
{
  double high;
  double low;
  double ratio;

  x = fabs(x);
  y = fabs(y);
  high = x > y ? x : y;
  low = x > y ? y : x;
  if (isinf(high))
    {
      return high;
    }

  if (high == 0.0)
    {
      return 0.0;
    }

  ratio = low / high;
  return high * sqrt(1.0 + ratio * ratio);
}

double log1p(double x)
{
  double y;

  if (x == -1.0)
    {
      return -INFINITY;
    }

  if (x < -1.0)
    {
      return NAN;
    }

  y = 1.0 + x;
  if (y == 1.0)
    {
      return x;
    }

  /* Correct the rounding lost while forming 1+x. */

  return log(y) - ((y - 1.0) - x) / y;
}
