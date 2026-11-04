// shim.c -- C support for repl.strata.
//
// Allocator hooks for Strata code, and a guard that lets a runtime panic in an
// entry return to the prompt instead of ending the process.

#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <termios.h>
#include <unistd.h>

#include "strata/strata.h"

void* strata_alloc(unsigned long n)
{
    return malloc((size_t)n);
}
void strata_free(void* p)
{
    free(p);
}

static jmp_buf guard;
static int guarded = 0;

static void on_panic(const char* msg)
{
    if (guarded)
    {
        fprintf(stderr, "runtime error: %s\n", msg);
        longjmp(guard, 1);
    }
    fprintf(stderr, "strata panic: %s\n", msg);
    abort();
}

void repl_install_panic_hook(void)
{
    strataSetPanicHandler(on_panic);
}

// Calls fn() and returns 0, or returns 1 if it panicked.
int repl_run_guarded(void* fn)
{
    if (setjmp(guard))
    {
        guarded = 0;
        return 1;
    }
    guarded = 1;
    ((void (*)(void))fn)();
    guarded = 0;
    return 0;
}

// Terminal modes for the line editor. Raw mode is on only while a line is read, so
// output, and Ctrl-C while an entry runs, behave normally.
static struct termios saved_termios;
static int raw_on = 0;

int repl_is_tty(void)
{
    return isatty(0);
}

void repl_raw_on(void)
{
    struct termios raw;

    if (raw_on || tcgetattr(0, &saved_termios) != 0)
    {
        return;
    }
    raw = saved_termios;
    raw.c_lflag &= ~(ICANON | ECHO | ISIG);
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;
    if (tcsetattr(0, TCSANOW, &raw) == 0)
    {
        raw_on = 1;
    }
}

void repl_raw_off(void)
{
    if (raw_on)
    {
        tcsetattr(0, TCSANOW, &saved_termios);
        raw_on = 0;
    }
}
