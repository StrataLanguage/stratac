// extern_layout_host.c -- host side of extern_layout.strata.
//
// Defines the REAL C structs the Strata `extern struct` declarations mirror,
// proves the layouts agree with static_asserts, and implements the extern
// functions. `^T` members are opaque pointers to the host — it never frees
// them (Strata owns and drops them).
//
//   stratac extern_layout.strata -o extern_layout.o
//   clang hosts/extern_layout_host.c extern_layout.o -o extern_layout.exe
//   ./extern_layout.exe
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>

/* Strata AOT runtime hooks the host provides. */
void* strata_alloc(unsigned long n) { return malloc((size_t)n); }
void strata_free(void* p) { free(p); }
void strata_panic(const char* msg)
{
    fprintf(stderr, "strata: %s\n", msg);
    abort();
}

/* Strata's `long` is 64-bit on every target (LLP64 hosts need long long). */
typedef struct { long long hits; } Counter;

typedef struct {
    int magic;                 /* @ 0  */
    /* 4 bytes of padding — mirrored by fieldoffset(8) */
    long long size;            /* @ 8  */
    unsigned char name[16];    /* @ 16 */
    float bbox[4];             /* @ 32 */
    int count;                 /* @ 48 */
} Header;

typedef struct {
    Counter* hits;             /* @ 0  — a Strata-owned box; opaque here */
    int cells[2][3];           /* @ 8  */
} Owned;

/* The Strata mirror must agree with the host compiler byte for byte. */
_Static_assert(offsetof(Header, magic) == 0, "magic offset");
_Static_assert(offsetof(Header, size) == 8, "size offset");
_Static_assert(offsetof(Header, name) == 16, "name offset");
_Static_assert(offsetof(Header, bbox) == 32, "bbox offset");
_Static_assert(offsetof(Header, count) == 48, "count offset");
_Static_assert(sizeof(Header) == 56, "Header size");
_Static_assert(offsetof(Owned, cells) == 8, "cells offset");
_Static_assert(sizeof(Owned) == 32, "Owned size");

long long sum_header(const Header* h)
{
    long long total = h->magic + h->size + h->count;
    for (int i = 0; i < 16; i++) total += h->name[i];
    for (int i = 0; i < 4; i++) total += (long long)h->bbox[i];
    return total;
}

void fill_header(Header* h)
{
    h->magic = 8;
    h->size = 16;
    for (int i = 0; i < 16; i++) h->name[i] = 0;
    h->name[0] = 83; /* 'S' */
    for (int i = 0; i < 4; i++) h->bbox[i] = 0.0f;
    h->bbox[3] = 4.0f;
    h->count = 32;
}

/* defined in Strata code. */
extern long long entry(void);

int main(void)
{
    printf("entry() = %lld\n", entry()); /* 385 */
    return 0;
}
