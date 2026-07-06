#ifndef SRE_STRING_H
#define SRE_STRING_H

#include <stddef.h>

void* memcpy(void* destination, const void* source, size_t num);
void* memmove(void* destination, const void* source, size_t num);
void* memset(void* ptr, int value, size_t num);
int memcmp(const void* ptr1, const void* ptr2, size_t num);
void* memchr(const void* ptr, int value, size_t num);

char* strcpy(char* destination, const char* source);
char* strncpy(char* destination, const char* source, size_t num);
char* strcat(char* destination, const char* source);
char* strncat(char* destination, const char* source, size_t num);
int strcmp(const char* str1, const char* str2);
int strncmp(const char* str1, const char* str2, size_t num);
size_t strlen(const char* str);
size_t strnlen(const char* s, size_t maxlen);
char* strchr(const char* str, int character);
char* strrchr(const char* str, int character);
size_t strcspn(const char* str1, const char* str2);
size_t strspn(const char* str1, const char* str2);
char* strpbrk(const char* str1, const char* str2);
char* strstr(const char* str1, const char* str2);
int strcoll(const char* str1, const char* str2);
char* strtok(char* str, const char* delimiters);
char* strerror(int errnum);

#endif
