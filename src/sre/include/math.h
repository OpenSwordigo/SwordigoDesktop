#ifndef SRE_MATH_H
#define SRE_MATH_H

double floor(double x);
double ceil(double x);
double fabs(double x);
double pow(double x, double y);
double log(double x);
double exp(double x);
double sqrt(double x);
double sin(double x);
double cos(double x);
double tan(double x);
double asin(double x);
double acos(double x);
double atan(double x);
double atan2(double y, double x);
double sinh(double x);
double cosh(double x);
double tanh(double x);
double fmod(double x, double y);
double modf(double x, double* iptr);
double log10(double x);
double frexp(double x, int* exp);
double ldexp(double x, int exp);

float floorf(float x);
float ceilf(float x);
float fabsf(float x);
float powf(float x, float y);
float logf(float x);
float expf(float x);
float sqrtf(float x);

int rand(void);
void srand(unsigned int seed);

#ifndef RAND_MAX
#define RAND_MAX 2147483647
#endif

#ifndef HUGE_VAL
#define HUGE_VAL (1.0 / 0.0)
#endif

int isnan(double x);
int isinf(double x);

#endif
