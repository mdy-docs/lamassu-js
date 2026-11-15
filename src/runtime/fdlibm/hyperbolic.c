/* From FreeBSD msun (fdlibm, which V8's src/base/ieee754.cc also derives from), adapted for
 * lamassu: no libm, js_k_ names, the original fdlibm overflow range for sinh/cosh (V8's, in
 * place of FreeBSD's __ldexp_exp). expm1, log1p, sinh, cosh, tanh, asinh, acosh, atanh. */
#include "libm.h"

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

static const double
one         = 1.0,
half        = 0.5,
two         = 2.0,
zero        = 0.0,
tiny        = 1.0e-300,
huge        = 1.0e+300,
o_threshold = 7.09782712893383973096e+02, /* 0x40862E42, 0xFEFA39EF */
ln2         = 6.93147180559945286227e-01, /* 0x3FE62E42, 0xFEFA39EF */
ln2_hi      = 6.93147180369123816490e-01, /* 0x3fe62e42, 0xfee00000 */
ln2_lo      = 1.90821492927058770002e-10, /* 0x3dea39ef, 0x35793c76 */
invln2      = 1.44269504088896338700e+00, /* 0x3ff71547, 0x652b82fe */
/* Scaled Q's: Qn_here = 2**n * Qn_above, for R(2*z) where z = hxs = x*x/2: */
Q1  =  -3.33333333333331316428e-02, /* BFA11111 111110F4 */
Q2  =   1.58730158725481460165e-03, /* 3F5A01A0 19FE5585 */
Q3  =  -7.93650757867487942473e-05, /* BF14CE19 9EAADBB7 */
Q4  =   4.00821782732936239552e-06, /* 3ED0CFCA 86E65239 */
Q5  =  -2.01099218183624371326e-07, /* BE8AFDB7 6E09C32D */
Lp1 = 6.666666666666735130e-01,  /* 3FE55555 55555593 */
Lp2 = 3.999999999940941908e-01,  /* 3FD99999 9997FA04 */
Lp3 = 2.857142874366239149e-01,  /* 3FD24924 94229359 */
Lp4 = 2.222219843214978396e-01,  /* 3FCC71C5 1D8E78AF */
Lp5 = 1.818357216161805012e-01,  /* 3FC74664 96CB03DE */
Lp6 = 1.531383769920937332e-01,  /* 3FC39A09 D078C69F */
Lp7 = 1.479819860511658591e-01;  /* 3FC2F112 DF3E5244 */

double js_k_expm1(double x)
{
	double y,hi,lo,c,t,e,hxs,hfx,r1,twopk;
	int32_t k,xsb;
	uint32_t hx;

	c = 0;
	GET_HIGH_WORD(hx,x);
	xsb = hx&0x80000000;		/* sign bit of x */
	hx &= 0x7fffffff;		/* high word of |x| */

    /* filter out huge and non-finite argument */
	if(hx >= 0x4043687A) {			/* if |x|>=56*ln2 */
	    if(hx >= 0x40862E42) {		/* if |x|>=709.78... */
		if(hx>=0x7ff00000) {
		    uint32_t low;
		    GET_LOW_WORD(low,x);
		    if(((hx&0xfffff)|low)!=0)
			return x+x;	 /* NaN */
		    else return (xsb==0)? x:-1.0;/* exp(+-inf)={inf,-1} */
		}
		if(x > o_threshold) return huge*huge; /* overflow */
	    }
	    if(xsb!=0) { /* x < -56*ln2, return -1.0 with inexact */
		return tiny-one;	/* return -1 */
	    }
	}

    /* argument reduction */
	if(hx > 0x3fd62e42) {		/* if  |x| > 0.5 ln2 */
	    if(hx < 0x3FF0A2B2) {	/* and |x| < 1.5 ln2 */
		if(xsb==0)
		    {hi = x - ln2_hi; lo =  ln2_lo;  k =  1;}
		else
		    {hi = x + ln2_hi; lo = -ln2_lo;  k = -1;}
	    } else {
		k  = (int32_t)(invln2*x+((xsb==0)?0.5:-0.5));
		t  = k;
		hi = x - t*ln2_hi;	/* t*ln2_hi is exact here */
		lo = t*ln2_lo;
	    }
	    x = hi - lo;
	    c  = (hi-x)-lo;
	}
	else if(hx < 0x3c900000) {	/* when |x|<2**-54, return x */
	    return x;
	}
	else k = 0;

    /* x is now in primary range */
	hfx = 0.5*x;
	hxs = x*hfx;
	r1 = one+hxs*(Q1+hxs*(Q2+hxs*(Q3+hxs*(Q4+hxs*Q5))));
	t  = 3.0-r1*hfx;
	e  = hxs*((r1-t)/(6.0 - x*t));
	if(k==0) return x - (x*e-hxs);		/* c is 0 */
	INSERT_WORDS(twopk,((uint32_t)(0x3ff+k))<<20,0);	/* 2^k */
	e  = (x*(e-c)-c);
	e -= hxs;
	if(k== -1) return 0.5*(x-e)-0.5;
	if(k==1) {
	    if(x < -0.25) return -2.0*(e-(x+0.5));
	    else	      return  one+2.0*(x-e);
	}
	if (k <= -2 || k>56) {   /* suffice to return exp(x)-1 */
	    y = one-(e-x);
	    if (k == 1024) y = y*2.0*0x1p1023;
	    else y = y*twopk;
	    return y-one;
	}
	t = one;
	if(k<20) {
	    SET_HIGH_WORD(t,0x3ff00000 - (0x200000>>k));  /* t=1-2^-k */
	    y = t-(e-x);
	    y = y*twopk;
	} else {
	    SET_HIGH_WORD(t,((0x3ff-k)<<20));	/* 2^-k */
	    y = x-(e+t);
	    y += one;
	    y = y*twopk;
	}
	return y;
}

double js_k_log1p(double x)
{
	double hfsq,f,c,s,z,R,u;
	int32_t k,hx,hu,ax;

	f = c = 0;
	hu = 0;
	GET_HIGH_WORD(hx,x);
	ax = hx&0x7fffffff;

	k = 1;
	if (hx < 0x3FDA827A) {			/* 1+x < sqrt(2)+ */
	    if(ax>=0x3ff00000) {		/* x <= -1.0 */
		if(x==-1.0) return -INFINITY;	/* log1p(-1)=-inf */
		else return NAN;		/* log1p(x<-1)=NaN */
	    }
	    if(ax<0x3e200000) {			/* |x| < 2**-29 */
		if(ax<0x3c900000)		/* |x| < 2**-54 */
		    return x;
		else
		    return x - x*x*0.5;
	    }
	    if(hx>0||hx<=((int32_t)0xbfd2bec4)) {
		k=0;f=x;hu=1;}		/* sqrt(2)/2- <= 1+x < sqrt(2)+ */
	}
	if (hx >= 0x7ff00000) return x+x;
	if(k!=0) {
	    if(hx<0x43400000) {
		u = 1.0+x;
		GET_HIGH_WORD(hu,u);
		k  = (hu>>20)-1023;
		c  = (k>0)? 1.0-(u-x):x-(u-1.0);/* correction term */
		c /= u;
	    } else {
		u  = x;
		GET_HIGH_WORD(hu,u);
		k  = (hu>>20)-1023;
		c  = 0;
	    }
	    hu &= 0x000fffff;
	    if(hu<0x6a09e) {			/* u ~< sqrt(2) */
		SET_HIGH_WORD(u,hu|0x3ff00000);	/* normalize u */
	    } else {
		k += 1;
		SET_HIGH_WORD(u,hu|0x3fe00000);	/* normalize u/2 */
		hu = (0x00100000-hu)>>2;
	    }
	    f = u-1.0;
	}
	hfsq=0.5*f*f;
	if(hu==0) {	/* |f| < 2**-20 */
	    if(f==zero) {
		if(k==0) {
		    return zero;
		} else {
		    c += k*ln2_lo;
		    return k*ln2_hi+c;
		}
	    }
	    R = hfsq*(1.0-0.66666666666666666*f);
	    if(k==0) return f-R; else
		     return k*ln2_hi-((R-(k*ln2_lo+c))-f);
	}
	s = f/(2.0+f);
	z = s*s;
	R = z*(Lp1+z*(Lp2+z*(Lp3+z*(Lp4+z*(Lp5+z*(Lp6+z*Lp7))))));
	if(k==0) return f-(hfsq-s*(hfsq+R)); else
		 return k*ln2_hi-((hfsq-(s*(hfsq+R)+(k*ln2_lo+c)))-f);
}

double js_k_sinh(double x)
{
	double t,h;
	int32_t ix,jx;

	GET_HIGH_WORD(jx,x);
	ix = jx&0x7fffffff;

    /* x is INF or NaN */
	if(ix>=0x7ff00000) return x+x;

	h = 0.5;
	if (jx<0) h = -h;
    /* |x| in [0,22], return sign(x)*0.5*(E+E/(E+1))) */
	if (ix < 0x40360000) {		/* |x|<22 */
	    if (ix<0x3e300000) 		/* |x|<2**-28 */
		return x;		/* sinh(tiny) = tiny */
	    t = js_k_expm1(__builtin_fabs(x));
	    if(ix<0x3ff00000) return h*(2.0*t-t*t/(t+one));
	    return h*(t+t/(t+one));
	}

    /* |x| in [22, log(maxdouble)] return 0.5*exp(|x|) */
	if (ix < 0x40862E42)  return h*js_k_exp(__builtin_fabs(x));

    /* |x| in [log(maxdouble), overflowthresold] */
	if (__builtin_fabs(x) <= 7.10475860073943863426e+02) {
	    double w = js_k_exp(half*__builtin_fabs(x));
	    t = h*w;
	    return t*w;
	}

    /* |x| > overflowthresold, sinh(x) overflow */
	return x*1.0e307;
}

double js_k_cosh(double x)
{
	double t,w;
	int32_t ix;

	GET_HIGH_WORD(ix,x);
	ix &= 0x7fffffff;

    /* x is INF or NaN */
	if(ix>=0x7ff00000) return x*x;

    /* |x| in [0,0.5*ln2], return 1+expm1(|x|)^2/(2*exp(|x|)) */
	if(ix<0x3fd62e43) {
	    t = js_k_expm1(__builtin_fabs(x));
	    w = one+t;
	    if (ix<0x3c800000) return w;	/* cosh(tiny) = 1 */
	    return one+(t*t)/(w+w);
	}

    /* |x| in [0.5*ln2,22], return (exp(|x|)+1/exp(|x|)/2; */
	if (ix < 0x40360000) {
		t = js_k_exp(__builtin_fabs(x));
		return half*t+half/t;
	}

    /* |x| in [22, log(maxdouble)] return half*exp(|x|) */
	if (ix < 0x40862E42)  return half*js_k_exp(__builtin_fabs(x));

    /* |x| in [log(maxdouble), overflowthresold] */
	if (__builtin_fabs(x) <= 7.10475860073943863426e+02) {
	    w = js_k_exp(half*__builtin_fabs(x));
	    t = half*w;
	    return t*w;
	}

    /* |x| > overflowthresold, cosh(x) overflow */
	return huge*huge;
}

double js_k_tanh(double x)
{
	double t,z;
	int32_t jx,ix;

	GET_HIGH_WORD(jx,x);
	ix = jx&0x7fffffff;

    /* x is INF or NaN */
	if(ix>=0x7ff00000) {
	    if (jx>=0) return one/x+one;    /* tanh(+-inf)=+-1 */
	    else       return one/x-one;    /* tanh(NaN) = NaN */
	}

    /* |x| < 22 */
	if (ix < 0x40360000) {		/* |x|<22 */
	    if (ix<0x3e300000) 		/* |x|<2**-28 */
		return x;		/* tanh(tiny) = tiny */
	    if (ix>=0x3ff00000) {	/* |x|>=1  */
		t = js_k_expm1(two*__builtin_fabs(x));
		z = one - two/(t+two);
	    } else {
		t = js_k_expm1(-two*__builtin_fabs(x));
		z= -t/(t+two);
	    }
    /* |x| >= 22, return +-1 */
	} else {
	    z = one - tiny;
	}
	return (jx>=0)? z: -z;
}

double js_k_asinh(double x)
{
	double t,w;
	int32_t hx,ix;
	GET_HIGH_WORD(hx,x);
	ix = hx&0x7fffffff;
	if(ix>=0x7ff00000) return x+x;	/* x is inf or NaN */
	if(ix< 0x3e300000)	/* |x|<2**-28 */
	    return x;
	if(ix>0x41b00000) {	/* |x| > 2**28 */
	    w = js_k_log(__builtin_fabs(x))+ln2;
	} else if (ix>0x40000000) {	/* 2**28 > |x| > 2.0 */
	    t = __builtin_fabs(x);
	    w = js_k_log(2.0*t+one/(__builtin_sqrt(x*x+one)+t));
	} else {		/* 2.0 > |x| > 2**-28 */
	    t = x*x;
	    w = js_k_log1p(__builtin_fabs(x)+t/(one+__builtin_sqrt(one+t)));
	}
	if(hx>0) return w; else return -w;
}

double js_k_acosh(double x)
{
	double t;
	int32_t hx;
	uint32_t lx;
	EXTRACT_WORDS(hx,lx,x);
	if(hx<0x3ff00000) {		/* x < 1 */
	    return NAN;
	} else if(hx >=0x41b00000) {	/* x > 2**28 */
	    if(hx >=0x7ff00000) {	/* x is inf of NaN */
		return x+x;
	    } else
		return js_k_log(x)+ln2;	/* acosh(huge)=log(2x) */
	} else if(((hx-0x3ff00000)|lx)==0) {
	    return 0.0;			/* acosh(1) = 0 */
	} else if (hx > 0x40000000) {	/* 2**28 > x > 2 */
	    t=x*x;
	    return js_k_log(2.0*x-one/(x+__builtin_sqrt(t-one)));
	} else {			/* 1<x<2 */
	    t = x-one;
	    return js_k_log1p(t+__builtin_sqrt(2.0*t+t*t));
	}
}

double js_k_atanh(double x)
{
	double t;
	int32_t hx,ix;
	uint32_t lx;
	EXTRACT_WORDS(hx,lx,x);
	ix = hx&0x7fffffff;
	if ((ix|((lx|(-lx))>>31))>0x3ff00000) /* |x|>1 */
	    return NAN;
	if(ix==0x3ff00000)
	    return x/zero;
	if(ix<0x3e300000) return x;	/* x<2**-28 */
	SET_HIGH_WORD(x,ix);
	if(ix<0x3fe00000) {		/* x < 0.5 */
	    t = x+x;
	    t = 0.5*js_k_log1p(t+t*x/(one-x));
	} else
	    t = 0.5*js_k_log1p((x+x)/(one-x));
	if(hx>=0) return t; else return -t;
}
