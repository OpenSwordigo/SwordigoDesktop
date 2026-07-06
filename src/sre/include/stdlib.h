#ifndef SRE_STDLIB_H
#define SRE_STDLIB_H

#include <stddef.h>

#define EXIT_SUCCESS 0
#define EXIT_FAILURE 1

void* malloc(size_t size);
void* calloc(size_t num, size_t size);
void* realloc(void* ptr, size_t size);
void free(void* ptr);
void exit(int status);
void abort(void);
int abs(int value);
long int labs(long int value);
long long int llabs(long long int value);
char* getenv(const char* name);
int system(const char* command);
int atoi(const char* str);
double atof(const char* str);
long int strtol(const char* str, char** endptr, int base);
long long int strtoll(const char* str, char** endptr, int base);
unsigned long int strtoul(const char* str, char** endptr, int base);
double strtod(const char* str, char** endptr);

#endif
