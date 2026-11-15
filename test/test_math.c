/*
 * The fdlibm-derived math kernel: values that agree with Node bit for bit (a
 * sample across every function and argument range), checked by exact
 * comparison of the double. Guards against a regression to the old series
 * approximations (tan(pi/4) was 0.9999999998) and against the compiler
 * contracting a*b+c into an FMA, which moves results by an ulp.
 */
#include <stdio.h>
#include <string.h>

#include "lamassu.h"
#include "js_bytecode.h"

static int checks_run, checks_failed;

static double call(const char *fn, double a, double b) {
    if (!strcmp(fn, "sin")) return js_k_sin(a);
    if (!strcmp(fn, "cos")) return js_k_cos(a);
    if (!strcmp(fn, "tan")) return js_k_tan(a);
    if (!strcmp(fn, "asin")) return js_k_asin(a);
    if (!strcmp(fn, "acos")) return js_k_acos(a);
    if (!strcmp(fn, "atan")) return js_k_atan(a);
    if (!strcmp(fn, "atan2")) return js_k_atan2(a, b);
    if (!strcmp(fn, "exp")) return js_k_exp(a);
    if (!strcmp(fn, "log")) return js_k_log(a);
    if (!strcmp(fn, "log2")) return js_k_log2(a);
    if (!strcmp(fn, "log10")) return js_k_log10(a);
    if (!strcmp(fn, "cbrt")) return js_k_cbrt(a);
    if (!strcmp(fn, "pow")) return js_k_pow(a, b);
    if (!strcmp(fn, "expm1")) return js_k_expm1(a);
    if (!strcmp(fn, "log1p")) return js_k_log1p(a);
    if (!strcmp(fn, "sinh")) return js_k_sinh(a);
    if (!strcmp(fn, "cosh")) return js_k_cosh(a);
    if (!strcmp(fn, "tanh")) return js_k_tanh(a);
    if (!strcmp(fn, "asinh")) return js_k_asinh(a);
    if (!strcmp(fn, "acosh")) return js_k_acosh(a);
    if (!strcmp(fn, "atanh")) return js_k_atanh(a);
    return 0;
}

static void check(const char *fn, double a, double b, double expect) {
    double got = call(fn, a, b);
    checks_run++;
    if (got != expect) {
        checks_failed++;
        fprintf(stderr, "FAIL %s(%.17g, %.17g) = %.17g, expected %.17g\n", fn, a, b, got, expect);
    }
}

int main(void) {
    check("sin", 1e-300, 0, 1e-300);
    check("sin", 1e-10, 0, 1e-10);
    check("sin", 0.25, 0, 0.24740395925452294);
    check("sin", 0.5, 0, 0.479425538604203);
    check("sin", 0.7071067811865476, 0, 0.6496369390800625);
    check("sin", 0.785398163397448, 0, 0.7071067811865472);
    check("sin", 1, 0, 0.8414709848078965);
    check("sin", 2, 0, 0.9092974268256817);
    check("sin", 2.5, 0, 0.5984721441039564);
    check("sin", 3, 0, 0.1411200080598672);
    check("sin", 3.14159265358979, 0, 3.2310891488651735e-15);
    check("sin", 4, 0, -0.7568024953079282);
    check("sin", 5.5, 0, -0.7055403255703919);
    check("sin", 7, 0, 0.6569865987187891);
    check("sin", 10, 0, -0.5440211108893698);
    check("sin", 12.345, 0, -0.2195669967379325);
    check("sin", 50, 0, -0.26237485370392877);
    check("sin", 100, 0, -0.5063656411097588);
    check("sin", 123.456, 0, -0.8039373685728239);
    check("sin", 1000, 0, 0.8268795405320025);
    check("sin", 100000, 0, 0.03574879797201651);
    check("sin", 10000000000, 0, -0.4875060250875107);
    check("sin", -0.3, 0, -0.29552020666133955);
    check("sin", -1, 0, -0.8414709848078965);
    check("sin", -2.75, 0, -0.38166099205233167);
    check("sin", -10, 0, 0.5440211108893698);
    check("sin", -100, 0, 0.5063656411097588);
    check("cos", 0.25, 0, 0.9689124217106447);
    check("cos", 0.5, 0, 0.8775825618903728);
    check("cos", 0.7071067811865476, 0, 0.7602445970756301);
    check("cos", 0.785398163397448, 0, 0.7071067811865478);
    check("cos", 1, 0, 0.5403023058681398);
    check("cos", 1.5, 0, 0.0707372016677029);
    check("cos", 2, 0, -0.4161468365471424);
    check("cos", 2.5, 0, -0.8011436155469337);
    check("cos", 3, 0, -0.9899924966004454);
    check("cos", 3.14159265358979, 0, -1);
    check("cos", 4, 0, -0.6536436208636119);
    check("cos", 5.5, 0, 0.70866977429126);
    check("cos", 7, 0, 0.7539022543433046);
    /* the case that motivated the kernel */
    check("tan", 0.78539816339744828, 0, 0.99999999999999989);
    check("sin", 0.78539816339744828, 0, 0.70710678118654746);
    /* expm1, log1p and the hyperbolics (src/runtime/fdlibm/hyperbolic.c), values from Node */
    check("expm1", 0.5, 0, 0.6487212707001282);
    check("expm1", -0.5, 0, -0.3934693402873666);
    check("expm1", 2, 0, 6.38905609893065);
    check("expm1", -2, 0, -0.8646647167633873);
    check("expm1", 1e-10, 0, 1.00000000005e-10);
    check("expm1", 0.9, 0, 1.4596031111569499);
    check("expm1", 1.5, 0, 3.481689070338065);
    check("expm1", 10, 0, 22025.465794806718);
    check("expm1", 22.5, 0, 5910522062.023291);
    check("expm1", 700, 0, 1.0142320547350045e+304);
    check("log1p", 0.5, 0, 0.4054651081081644);
    check("log1p", -0.5, 0, -0.6931471805599453);
    check("log1p", 2, 0, 1.0986122886681096);
    check("log1p", 1e-10, 0, 9.999999999500001e-11);
    check("log1p", 0.9, 0, 0.6418538861723948);
    check("log1p", 1.5, 0, 0.9162907318741551);
    check("log1p", 10, 0, 2.3978952727983707);
    check("log1p", 22.5, 0, 3.157000421150113);
    check("log1p", 700, 0, 6.55250788703459);
    check("sinh", 0.5, 0, 0.5210953054937474);
    check("sinh", -0.5, 0, -0.5210953054937474);
    check("sinh", 2, 0, 3.626860407847019);
    check("sinh", -2, 0, -3.626860407847019);
    check("sinh", 1e-10, 0, 1e-10);
    check("sinh", 0.9, 0, 1.0265167257081753);
    check("sinh", 1.5, 0, 2.1292794550948173);
    check("sinh", 10, 0, 11013.232874703393);
    check("sinh", 22.5, 0, 2955261031.5116453);
    check("sinh", 700, 0, 5.0711602736750225e+303);
    check("cosh", 0.5, 0, 1.1276259652063807);
    check("cosh", -0.5, 0, 1.1276259652063807);
    check("cosh", 2, 0, 3.7621956910836314);
    check("cosh", -2, 0, 3.7621956910836314);
    check("cosh", 1e-10, 0, 1);
    check("cosh", 0.9, 0, 1.4330863854487745);
    check("cosh", 1.5, 0, 2.352409615243247);
    check("cosh", 10, 0, 11013.232920103324);
    check("cosh", 22.5, 0, 2955261031.5116453);
    check("cosh", 700, 0, 5.0711602736750225e+303);
    check("tanh", 0.5, 0, 0.46211715726000974);
    check("tanh", -0.5, 0, -0.46211715726000974);
    check("tanh", 2, 0, 0.9640275800758169);
    check("tanh", -2, 0, -0.9640275800758169);
    check("tanh", 1e-10, 0, 1e-10);
    check("tanh", 0.9, 0, 0.7162978701990245);
    check("tanh", 1.5, 0, 0.9051482536448664);
    check("tanh", 10, 0, 0.9999999958776927);
    check("tanh", 22.5, 0, 1);
    check("tanh", 700, 0, 1);
    check("asinh", 0.5, 0, 0.48121182505960347);
    check("asinh", -0.5, 0, -0.48121182505960347);
    check("asinh", 2, 0, 1.4436354751788103);
    check("asinh", -2, 0, -1.4436354751788103);
    check("asinh", 1e-10, 0, 1e-10);
    check("asinh", 0.9, 0, 0.8088669356527824);
    check("asinh", 1.5, 0, 1.1947632172871094);
    check("asinh", 10, 0, 2.99822295029797);
    check("asinh", 22.5, 0, 3.8071559515338214);
    check("asinh", 700, 0, 7.2442280258070415);
    check("acosh", 2, 0, 1.3169578969248166);
    check("acosh", 1.5, 0, 0.9624236501192069);
    check("acosh", 10, 0, 2.993222846126381);
    check("acosh", 22.5, 0, 3.806168296409984);
    check("acosh", 700, 0, 7.244227005398878);
    check("atanh", 0.5, 0, 0.5493061443340548);
    check("atanh", -0.5, 0, -0.5493061443340548);
    check("atanh", 1e-10, 0, 1e-10);
    check("atanh", 0.9, 0, 1.4722194895832204);
    printf("test_math: %d checks, %d failed\n", checks_run, checks_failed);
    return checks_failed ? 1 : 0;
}
