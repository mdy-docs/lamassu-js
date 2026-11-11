/* From musl libc 1.2.5 src/internal/floatscan.c (MIT, see NOTICE): the decimal path (decfloat,
 * scanexp). Adapted for lamassu: it reads UTF-16 text already checked by the lexer or Number()
 * (digits, an optional '.', digits, an optional exponent) instead of a FILE; it works in double
 * with the constants musl uses where long double is double (LDBL_MANT_DIG == 53), so the result is
 * correctly rounded the same way everywhere instead of depending on the C library's strtod (the
 * Windows one is not, next to powers of two); hexadecimal, inf/nan, errno and the sign are left out;
 * fmod by a power of two, copysign and scalbn are done exactly in place; brackets added where GCC's
 * -Wparentheses asks. */
#include "libm.h"

#include <limits.h>

#define LD_B1B_DIG 2
#define LD_B1B_MAX 9007199, 254740991
#define KMAX 128
#define MASK (KMAX-1)

typedef struct {
	const uint16_t *s;
	size_t n, i;
} Text;

static int shgetc(Text *f) { int c = f->i < f->n ? f->s[f->i] : -1; f->i++; return c; }
static void shunget(Text *f) { f->i--; }
static int isdig(int c) { return (unsigned)(c-'0') < 10; }

/* y mod 2^m for an integer-valued y with |y| < 2^63, exactly (fmod's sign: the dividend's) */
static double fmod_pow2(double y, int m)
{
	int64_t v = (int64_t)y, d = (int64_t)1 << m;
	return (double)(v % d);
}

static long long scanexp(Text *f)
{
	int c;
	int x;
	long long y;
	int neg = 0;

	c = shgetc(f);
	if (c=='+' || c=='-') {
		neg = (c=='-');
		c = shgetc(f);
	}
	if (!isdig(c)) {
		shunget(f);
		return LLONG_MIN;
	}
	for (x=0; isdig(c) && x<INT_MAX/10; c = shgetc(f))
		x = 10*x + c-'0';
	for (y=x; isdig(c) && y<LLONG_MAX/100; c = shgetc(f))
		y = 10*y + c-'0';
	for (; isdig(c); c = shgetc(f));
	shunget(f);
	return neg ? -y : y;
}

static double decfloat(Text *f, int c, int bits, int emin)
{
	uint32_t x[KMAX];
	static const uint32_t th[] = { LD_B1B_MAX };
	int i, j, k, a, z;
	long long lrp=0, dc=0;
	long long e10=0;
	int lnz = 0;
	int gotdig = 0, gotrad = 0;
	int rp;
	int e2;
	int emax = -emin-bits+3;
	int denormal = 0;
	double y;
	double frac=0;
	double bias=0;
	static const int p10s[] = { 10, 100, 1000, 10000,
		100000, 1000000, 10000000, 100000000 };

	j=0;
	k=0;

	/* Don't let leading zeros consume buffer space */
	for (; c=='0'; c = shgetc(f)) gotdig=1;
	if (c=='.') {
		gotrad = 1;
		for (c = shgetc(f); c=='0'; c = shgetc(f)) gotdig=1, lrp--;
	}

	x[0] = 0;
	for (; isdig(c) || c=='.'; c = shgetc(f)) {
		if (c == '.') {
			if (gotrad) break;
			gotrad = 1;
			lrp = dc;
		} else if (k < KMAX-3) {
			dc++;
			if (c!='0') lnz = (int)dc;
			if (j) x[k] = x[k]*10 + (uint32_t)(c-'0');
			else x[k] = (uint32_t)(c-'0');
			if (++j==9) {
				k++;
				j=0;
			}
			gotdig=1;
		} else {
			dc++;
			if (c!='0') {
				lnz = (KMAX-4)*9;
				x[KMAX-4] |= 1;
			}
		}
	}
	if (!gotrad) lrp=dc;

	if (gotdig && (c|32)=='e') {
		e10 = scanexp(f);
		if (e10 == LLONG_MIN) e10 = 0;
		lrp += e10;
	}
	if (!gotdig) return 0;

	/* Handle zero specially to avoid nasty special cases later */
	if (!x[0]) return 0.0;

	/* Optimize small integers (w/no exponent) and over/under-flow */
	if (lrp==dc && dc<10 && (bits>30 || x[0]>>bits==0))
		return (double)x[0];
	if (lrp > -emin/2)
		return INFINITY;
	if (lrp < emin-2*DBL_MANT_DIG)
		return 0.0;

	/* Align incomplete final B1B digit */
	if (j) {
		for (; j<9; j++) x[k]*=10;
		k++;
		j=0;
	}

	a = 0;
	z = k;
	e2 = 0;
	rp = (int)lrp;

	/* Optimize small to mid-size integers (even in exp. notation) */
	if (lnz<9 && lnz<=rp && rp < 18) {
		if (rp == 9) return (double)x[0];
		if (rp < 9) return (double)x[0] / p10s[8-rp];
		int bitlim = bits-3*(int)(rp-9);
		if (bitlim>30 || x[0]>>bitlim==0)
			return (double)x[0] * p10s[rp-10];
	}

	/* Drop trailing zeros */
	for (; !x[z-1]; z--);

	/* Align radix point to B1B digit boundary */
	if (rp % 9) {
		int rpm9 = rp>=0 ? rp%9 : rp%9+9;
		int p10 = p10s[8-rpm9];
		uint32_t carry = 0;
		for (k=a; k!=z; k++) {
			uint32_t tmp = x[k] % (uint32_t)p10;
			x[k] = x[k]/(uint32_t)p10 + carry;
			carry = 1000000000/(uint32_t)p10 * tmp;
			if (k==a && !x[k]) {
				a = ((a+1) & MASK);
				rp -= 9;
			}
		}
		if (carry) x[z++] = carry;
		rp += 9-rpm9;
	}

	/* Upscale until desired number of bits are left of radix point */
	while (rp < 9*LD_B1B_DIG || (rp == 9*LD_B1B_DIG && x[a]<th[0])) {
		uint32_t carry = 0;
		e2 -= 29;
		for (k=((z-1) & MASK); ; k=((k-1) & MASK)) {
			uint64_t tmp = ((uint64_t)x[k] << 29) + carry;
			if (tmp > 1000000000) {
				carry = (uint32_t)(tmp / 1000000000);
				x[k] = (uint32_t)(tmp % 1000000000);
			} else {
				carry = 0;
				x[k] = (uint32_t)tmp;
			}
			if (k==((z-1) & MASK) && k!=a && !x[k]) z = k;
			if (k==a) break;
		}
		if (carry) {
			rp += 9;
			a = ((a-1) & MASK);
			if (a == z) {
				z = ((z-1) & MASK);
				x[(z-1) & MASK] |= x[z];
			}
			x[a] = carry;
		}
	}

	/* Downscale until exactly number of bits are left of radix point */
	for (;;) {
		uint32_t carry = 0;
		int sh = 1;
		for (i=0; i<LD_B1B_DIG; i++) {
			k = ((a+i) & MASK);
			if (k == z || x[k] < th[i]) {
				i=LD_B1B_DIG;
				break;
			}
			if (x[(a+i) & MASK] > th[i]) break;
		}
		if (i==LD_B1B_DIG && rp==9*LD_B1B_DIG) break;
		/* FIXME: find a way to compute optimal sh */
		if (rp > 9+9*LD_B1B_DIG) sh = 9;
		e2 += sh;
		for (k=a; k!=z; k=((k+1) & MASK)) {
			uint32_t tmp = x[k] & ((1u<<sh)-1);
			x[k] = (x[k]>>sh) + carry;
			carry = (1000000000u>>sh) * tmp;
			if (k==a && !x[k]) {
				a = ((a+1) & MASK);
				i--;
				rp -= 9;
			}
		}
		if (carry) {
			if (((z+1) & MASK) != a) {
				x[z] = carry;
				z = ((z+1) & MASK);
			} else x[(z-1) & MASK] |= 1;
		}
	}

	/* Assemble desired bits into floating point variable */
	for (y=i=0; i<LD_B1B_DIG; i++) {
		if (((a+i) & MASK)==z) x[(z=((z+1) & MASK))-1] = 0;
		y = 1000000000.0 * y + x[(a+i) & MASK];
	}

	/* Limit precision for denormal results */
	if (bits > DBL_MANT_DIG+e2-emin) {
		bits = DBL_MANT_DIG+e2-emin;
		if (bits<0) bits=0;
		denormal = 1;
	}

	/* Calculate bias term to force rounding, move out lower bits */
	if (bits < DBL_MANT_DIG) {
		bias = k_scalbn(1, 2*DBL_MANT_DIG-bits-1); /* y > 0: copysign(.., y) is the value itself */
		frac = fmod_pow2(y, DBL_MANT_DIG-bits);
		y -= frac;
		y += bias;
	}

	/* Process tail of decimal input so it can affect rounding */
	if (((a+i) & MASK) != z) {
		uint32_t t = x[(a+i) & MASK];
		if (t < 500000000 && (t || ((a+i+1) & MASK) != z))
			frac += 0.25;
		else if (t > 500000000)
			frac += 0.75;
		else if (t == 500000000) {
			if (((a+i+1) & MASK) == z)
				frac += 0.5;
			else
				frac += 0.75;
		}
		if (DBL_MANT_DIG-bits >= 2 && frac == (double)(int64_t)frac)
			frac++;
	}

	y += frac;
	y -= bias;

	if (((e2+DBL_MANT_DIG) & INT_MAX) > emax-5) {
		if (__builtin_fabs(y) >= 2/DBL_EPSILON) {
			if (denormal && bits==DBL_MANT_DIG+e2-emin)
				denormal = 0;
			y *= 0.5;
			e2++;
		}
	}

	return k_scalbn(y, e2);
}

double js_k_decimal(const uint16_t *s, size_t n)
{
	Text f = { s, n, 0 };
	int bits = DBL_MANT_DIG, emin = DBL_MIN_EXP-bits;
	return decfloat(&f, shgetc(&f), bits, emin);
}
