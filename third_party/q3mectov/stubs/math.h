/* <math.h> stub — ioquake3 subset on Mectov OS kernel (v38.98).
 * x87 FPU available (eager fxsave switching since v38.41). Libm subset
 * implemented in q3_kernel.c over inline x87 where it matters.
 */
#ifndef Q3STUB_MATH_H
#define Q3STUB_MATH_H

double q3_sin(double x);
double q3_cos(double x);
double q3_tan(double x);
double q3_sqrt(double x);
double q3_fabs(double x);
double q3_floor(double x);
double q3_ceil(double x);
double q3_fmod(double x, double y);
double q3_atan(double x);
double q3_atan2(double y, double x);
double q3_acos(double x);
double q3_asin(double x);
double q3_pow(double x, double y);
double q3_exp(double x);
double q3_log(double x);
float  q3_sqrtf(float x);
float  q3_fabsf(float x);
float  q3_floorf(float x);
float  q3_ceilf(float x);
float  q3_fmodf(float x, float y);
float  q3_atan2f(float y, float x);
float  q3_sinf(float x);
float  q3_cosf(float x);
int    q3_isfinite(double x);

#define sin    q3_sin
#define isfinite q3_isfinite
#define cos    q3_cos
#define tan    q3_tan
#define sqrt   q3_sqrt
#define fabs   q3_fabs
#define floor  q3_floor
#define ceil   q3_ceil
#define fmod   q3_fmod
#define atan   q3_atan
#define atan2  q3_atan2
#define acos   q3_acos
#define asin   q3_asin
#define pow    q3_pow
#define exp    q3_exp
#define log    q3_log
#define sqrtf  q3_sqrtf
#define fabsf  q3_fabsf
#define floorf q3_floorf
#define ceilf  q3_ceilf
#define fmodf  q3_fmodf
#define atan2f q3_atan2f
#define sinf   q3_sinf
#define cosf   q3_cosf

#endif /* Q3STUB_MATH_H */
