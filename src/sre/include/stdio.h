#ifndef SRE_STDIO_H
#define SRE_STDIO_H

#include <stddef.h>
#include <stdarg.h>

typedef struct FILE FILE;

#ifndef NULL
#define NULL ((void*)0)
#endif

#define EOF (-1)
#define BUFSIZ 8192
#define _IONBF 0
#define _IOFBF 1
#define _IOLBF 2

#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2
#define L_tmpnam 25

extern FILE* stdin;
extern FILE* stdout;
extern FILE* stderr;

FILE* fopen(const char* filename, const char* mode);
int fclose(FILE* stream);
size_t fread(void* ptr, size_t size, size_t nmemb, FILE* stream);
size_t fwrite(const void* ptr, size_t size, size_t nmemb, FILE* stream);
int fseek(FILE* stream, long int offset, int whence);
long int ftell(FILE* stream);
int fflush(FILE* stream);
FILE* tmpfile(void);
char* tmpnam(char* buffer);
void clearerr(FILE* stream);
int setvbuf(FILE* stream, char* buffer, int mode, size_t size);
char* fgets(char* str, int num, FILE* stream);
int fscanf(FILE* stream, const char* format, ...);
int fprintf(FILE* stream, const char* format, ...);
int sprintf(char* str, const char* format, ...);
int snprintf(char* str, size_t size, const char* format, ...);
int printf(const char* format, ...);
int fputs(const char* str, FILE* stream);
int fputc(int character, FILE* stream);
int fgetc(FILE* stream);
int putc(int character, FILE* stream);
int getc(FILE* stream);
int feof(FILE* stream);
int ferror(FILE* stream);
int ungetc(int character, FILE* stream);
int fileno(FILE* stream);
void perror(const char* str);
int remove(const char* filename);
int rename(const char* oldname, const char* newname);

#endif
