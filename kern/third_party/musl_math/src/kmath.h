/* Kern: what these musl sources took from <math.h> and <features.h>, for a libm that gives the same
 * bits on every platform (x86, ARM, WebAssembly). The functions are renamed kern_musl_* so they never
 * meet the platform's own libm; there are no FMA paths (__FP_FAST_FMA is 0 everywhere), and
 * float_t / double_t are float / double (FLT_EVAL_METHOD 0 on SSE2, ARM64 and wasm). */
#ifndef KERN_MUSL_KMATH_H
#define KERN_MUSL_KMATH_H

#include <stdint.h>
#include <float.h>

#undef __FP_FAST_FMA
#define __FP_FAST_FMA 0
#define hidden __attribute__((__visibility__("hidden")))
#define weak_alias(old, new)

typedef float float_t;
typedef double double_t;

#define M_PI_2 1.57079632679489661923
#define INFINITY __builtin_inff()
#define NAN __builtin_nanf("")
#define HUGE_VAL __builtin_huge_val()
#define isnan(x) __builtin_isnan(x)
#define isinf(x) __builtin_isinf(x)
#define signbit(x) __builtin_signbit(x)

/* Exact operations: every IEEE platform gives the same bits. */
#define fabs(x) __builtin_fabs(x)
#define fabsf(x) __builtin_fabsf(x)
#define sqrt(x) __builtin_sqrt(x)
#define sqrtf(x) __builtin_sqrtf(x)
#define floor(x) __builtin_floor(x)
#define floorf(x) __builtin_floorf(x)
#define scalbn(x, n) __builtin_scalbn(x, n)
#define scalbnf(x, n) __builtin_scalbnf(x, n)

/* The functions Kern takes from here (kern/core/dmath.h), and musl's internals they use. */
#define sin kern_musl_sin
#define cos kern_musl_cos
#define tan kern_musl_tan
#define asin kern_musl_asin
#define acos kern_musl_acos
#define atan kern_musl_atan
#define atan2 kern_musl_atan2
#define exp kern_musl_exp
#define expm1 kern_musl_expm1
#define log kern_musl_log
#define log10 kern_musl_log10
#define pow kern_musl_pow
#define hypot kern_musl_hypot
#define sinf kern_musl_sinf
#define cosf kern_musl_cosf
#define tanf kern_musl_tanf
#define asinf kern_musl_asinf
#define acosf kern_musl_acosf
#define atanf kern_musl_atanf
#define atan2f kern_musl_atan2f
#define expf kern_musl_expf
#define expm1f kern_musl_expm1f
#define logf kern_musl_logf
#define log10f kern_musl_log10f
#define powf kern_musl_powf
#define hypotf kern_musl_hypotf
#define __sin kern_musl___sin
#define __cos kern_musl___cos
#define __tan kern_musl___tan
#define __sindf kern_musl___sindf
#define __cosdf kern_musl___cosdf
#define __tandf kern_musl___tandf
#define __rem_pio2 kern_musl___rem_pio2
#define __rem_pio2f kern_musl___rem_pio2f
#define __rem_pio2_large kern_musl___rem_pio2_large
#define __exp_data kern_musl___exp_data
#define __exp2f_data kern_musl___exp2f_data
#define __log_data kern_musl___log_data
#define __logf_data kern_musl___logf_data
#define __pow_log_data kern_musl___pow_log_data
#define __powf_log2_data kern_musl___powf_log2_data
#define __math_oflow kern_musl___math_oflow
#define __math_uflow kern_musl___math_uflow
#define __math_xflow kern_musl___math_xflow
#define __math_invalid kern_musl___math_invalid
#define __math_divzero kern_musl___math_divzero
#define __math_oflowf kern_musl___math_oflowf
#define __math_uflowf kern_musl___math_uflowf
#define __math_xflowf kern_musl___math_xflowf
#define __math_invalidf kern_musl___math_invalidf
#define __math_divzerof kern_musl___math_divzerof

double sin(double); double cos(double); double tan(double);
double asin(double); double acos(double); double atan(double); double atan2(double, double);
double exp(double); double expm1(double); double log(double); double log10(double);
double pow(double, double); double hypot(double, double);
float sinf(float); float cosf(float); float tanf(float);
float asinf(float); float acosf(float); float atanf(float); float atan2f(float, float);
float expf(float); float expm1f(float); float logf(float); float log10f(float);
float powf(float, float); float hypotf(float, float);

#endif
