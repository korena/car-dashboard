#ifndef _TERMINAL_H_
#define _TERMINAL_H_

#include <stdint.h>
#include <stdio.h>
/************** function prototypes ******************/

/*
 * Not strictly required, but gives me an idea of where things are ...
 * These are globalized in asm/uart_mod.s
 */

extern void uart_print(const char* str);
extern void uart_print_hex(uint32_t integer);


int printf(const char* fmt, ...); // MISRA C hates this :-D
char *strcpy(char *strDest, const char *strSrc);
int snprintf (char *__restrict__ s, size_t maxlen, const char *__restrict__ format, ...);
#endif