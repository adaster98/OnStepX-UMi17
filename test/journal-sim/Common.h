// Minimal stand-in for OnStepX/src/Common.h, host side.
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>

#define ON 1
#define OFF 0
#define JOURNAL ON
#define JOURNAL_RUNWAY_SECTORS 16
#define JOURNAL_PARTITION "journal"

extern int g_verbose;
#define VLF(x)  do { if (g_verbose) printf("%s\n", x); } while(0)
#define VF(x)   do { if (g_verbose) printf("%s", x); } while(0)
#define V(x)    do { if (g_verbose) std::cout << x; } while(0)
#define VL(x)   do { if (g_verbose) std::cout << x << "\n"; } while(0)
#define DLF(x)  do { printf("%s\n", x); } while(0)
#define DF(x)   do { printf("%s", x); } while(0)
#define D(x)    do { std::cout << x; } while(0)
#include <iostream>

unsigned long millis();
