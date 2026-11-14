#ifndef NOVA_LIBC_MATH_H
#define NOVA_LIBC_MATH_H

double fabs(double x);
double sqrt(double x);
double floor(double x);
double ceil(double x);
double sin(double x);
double cos(double x);
double tan(double x);
double atan(double x);
double atan2(double y, double x);

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#endif
