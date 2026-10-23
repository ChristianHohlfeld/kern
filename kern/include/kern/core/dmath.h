// Deterministic transcendental functions: the same bits on every platform Kern runs on (x86 and ARM
// natively, WebAssembly in every browser), so a simulation, a training run or a replayed robot log
// comes out identical everywhere (docs/lehrer.md, determinism).
//
// The platform's own libm (glibc, Apple's, Android's bionic, wasi-libc) rounds sin, exp, atan2 ...
// differently in the last bit; a robot simulation amplifies one bit into a different walk within
// seconds. These are musl's implementations (MIT, kern/third_party/musl_math), compiled the same way
// everywhere: no FMA contraction, no platform-specific paths. sqrt, fabs and floor are exact in IEEE
// arithmetic and stay std::.
//
// Use kern::dm:: instead of std:: for sin, cos, tan, asin, acos, atan, atan2, exp, expm1, log,
// log10, pow and hypot in everything that feeds the simulation.
#pragma once

#include <type_traits>

extern "C" {
double kern_musl_sin(double); double kern_musl_cos(double); double kern_musl_tan(double);
double kern_musl_asin(double); double kern_musl_acos(double); double kern_musl_atan(double);
double kern_musl_atan2(double, double); double kern_musl_exp(double); double kern_musl_expm1(double);
double kern_musl_log(double); double kern_musl_log10(double); double kern_musl_pow(double, double);
double kern_musl_hypot(double, double);
float kern_musl_sinf(float); float kern_musl_cosf(float); float kern_musl_tanf(float);
float kern_musl_asinf(float); float kern_musl_acosf(float); float kern_musl_atanf(float);
float kern_musl_atan2f(float, float); float kern_musl_expf(float); float kern_musl_expm1f(float);
float kern_musl_logf(float); float kern_musl_log10f(float); float kern_musl_powf(float, float);
float kern_musl_hypotf(float, float);
}

namespace kern::dm {

#define KERN_DM_UNARY(name)                                                         \
  inline float name(float x) { return kern_musl_##name##f(x); }                     \
  inline double name(double x) { return kern_musl_##name(x); }                      \
  template <class T, class = std::enable_if_t<std::is_integral_v<T>>>               \
  inline double name(T x) { return kern_musl_##name(static_cast<double>(x)); }
KERN_DM_UNARY(sin)
KERN_DM_UNARY(cos)
KERN_DM_UNARY(tan)
KERN_DM_UNARY(asin)
KERN_DM_UNARY(acos)
KERN_DM_UNARY(atan)
KERN_DM_UNARY(exp)
KERN_DM_UNARY(expm1)
KERN_DM_UNARY(log)
KERN_DM_UNARY(log10)
#undef KERN_DM_UNARY

// Two arguments promote as std:: does: both float -> float, anything else -> double.
#define KERN_DM_BINARY(name)                                                                          \
  inline float name(float a, float b) { return kern_musl_##name##f(a, b); }                           \
  template <class A, class B, class = std::enable_if_t<std::is_arithmetic_v<A> && std::is_arithmetic_v<B> && \
                                                       !(std::is_same_v<A, float> && std::is_same_v<B, float>)>> \
  inline double name(A a, B b) { return kern_musl_##name(static_cast<double>(a), static_cast<double>(b)); }
KERN_DM_BINARY(atan2)
KERN_DM_BINARY(pow)
KERN_DM_BINARY(hypot)
#undef KERN_DM_BINARY

}  // namespace kern::dm
