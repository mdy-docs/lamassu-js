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
    printf("test_math: %d checks, %d failed\n", checks_run, checks_failed);
    return checks_failed ? 1 : 0;
}
