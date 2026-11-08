/* From musl libc (MIT; fdlibm heritage, see the notice below). Adapted for lamassu: no libm, js_k_/k_ names. */
#include "libm.h"

/* origin: FreeBSD /usr/src/lib/msun/src/s_sin.c */
/*
 * ====================================================
 * Copyright (C) 1993 by Sun Microsystems, Inc. All rights reserved.
 *
 * Developed at SunPro, a Sun Microsystems, Inc. business.
 * Permission to use, copy, modify, and distribute this
 * software is freely granted, provided that this notice
 * is preserved.
 * ====================================================
 */
/* js_k_sin(x)
 * Return sine function of x.
 *
 * kernel function:
 *      k_sin            ... sine function on [-pi/4,pi/4]
 *      k_cos            ... cose function on [-pi/4,pi/4]
 *      k_rem_pio2       ... argument reduction routine
 *
 * Method.
 *      Let S,C and T denote the sin, cos and tan respectively on
 *      [-PI/4, +PI/4]. Reduce the argument x to y1+y2 = x-k*pi/2
 *      in [-pi/4 , +pi/4], and let n = k mod 4.
 *      We have
 *
 *          n        js_k_sin(x)      js_k_cos(x)        js_k_tan(x)
 *     ----------------------------------------------------------
 *          0          S           C             T
 *          1          C          -S            -1/T
 *          2         -S          -C             T
 *          3         -C           S            -1/T
 *     ----------------------------------------------------------
 *
 * Special cases:
 *      Let trig be any of sin, cos, or tan.
 *      trig(+-INF)  is NaN, with signals;
 *      trig(NaN)    is that NaN;
 *
 * Accuracy:
 *      TRIG(x) returns trig(x) nearly rounded
 */


double js_k_sin(double x)
{
	double y[2];
	uint32_t ix;
	unsigned n;

	/* High word of x. */
	GET_HIGH_WORD(ix, x);
	ix &= 0x7fffffff;

	/* |x| ~< pi/4 */
	if (ix <= 0x3fe921fb) {
		if (ix < 0x3e500000) {  /* |x| < 2**-26 */
			/* raise inexact if x != 0 and underflow if subnormal*/
			FORCE_EVAL(ix < 0x00100000 ? x/0x1p120f : x+0x1p120f);
			return x;
		}
		return k_sin(x, 0.0, 0);
	}

	/* js_k_sin(Inf or NaN) is NaN */
	if (ix >= 0x7ff00000)
		return x - x;

	/* argument reduction needed */
	n = k_rem_pio2(x, y);
	switch (n&3) {
	case 0: return  k_sin(y[0], y[1], 1);
	case 1: return  k_cos(y[0], y[1]);
	case 2: return -k_sin(y[0], y[1], 1);
	default:
		return -k_cos(y[0], y[1]);
	}
}
