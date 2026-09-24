// components_host.c -- links two copies of components.strata, built with the symbol
// prefixes Alpha_ and Beta_, and reads each one's component metadata.
#include "strata/strata.h"
#include "strata/strata_types.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Strata runtime: box/string heap allocation
void* strata_alloc(unsigned long n) { return malloc((size_t)n); }
void strata_free(void* p) { free(p); }
void strata_panic(const char* msg) { fprintf(stderr, "strata panic: %s\n", msg); abort(); }

extern const unsigned char Alpha___strata_types[];
extern void* Alpha___strata_context_create(void);
extern void Alpha___strata_context_destroy(void*);
extern int Alpha_take_hit(void* ctx, int amount);

extern const unsigned char Beta___strata_types[];
extern void* Beta___strata_context_create(void);
extern void Beta___strata_context_destroy(void*);
extern int Beta_take_hit(void* ctx, int amount);

// extern<T> bool HostRead(ref T value): both objects call this one function.
bool HostRead(void* value, const StrataTypeDesc* type)
{
    if (strcmp(strataTypeDescName(type), "Health") != 0 || type->size < sizeof(float))
    {
        return false;
    }

    float current = 60.0f;
    memcpy(value, &current, sizeof(current));

    return true;
}

/* The blob's own header carries its size. */
static int OpenEmbedded(StrataTypes* types, const unsigned char* blob)
{
    uint32_t size = (uint32_t)blob[8] | ((uint32_t)blob[9] << 8) | ((uint32_t)blob[10] << 16) | ((uint32_t)blob[11] << 24);

    return strataTypesOpen(types, blob, size);
}

static int CheckHealth(const char* label, const unsigned char* blob)
{
    StrataTypes types;
    StrataTypesStruct health;
    StrataTypesField field;

    if (!OpenEmbedded(&types, blob) || strataTypesStructCount(&types) != 1 || !strataTypesGetStruct(&types, 0, &health))
    {
        printf("%s: unreadable metadata\n", label);
        return 0;
    }

    if (strcmp(health.name, "Health") != 0 || !(health.flags & STRATA_TYPES_STRUCT_COMPONENT) || health.fieldCount != 3)
    {
        printf("%s: unexpected struct '%s'\n", label, health.name);
        return 0;
    }

    float maximum = 0.0f;

    if (!strataTypesGetField(&types, &health, 1, &field) || strcmp(field.name, "maximum") != 0)
    {
        printf("%s: missing field 'maximum'\n", label);
        return 0;
    }

    memcpy(&maximum, (const unsigned char*)health.defaultValue + field.offset, sizeof(maximum));

    if (maximum != 150.0f)
    {
        printf("%s: maximum defaults to %f\n", label, maximum);
        return 0;
    }

    return 1;
}

int main(void)
{
    if (!CheckHealth("Alpha", Alpha___strata_types) || !CheckHealth("Beta", Beta___strata_types))
    {
        return 1;
    }

    void* alpha = Alpha___strata_context_create();
    void* beta = Beta___strata_context_create();

    // Separate objects, separate state: Alpha takes two hits, Beta one.
    Alpha_take_hit(alpha, 10);
    int alphaResult = Alpha_take_hit(alpha, 10);
    int betaResult = Beta_take_hit(beta, 10);

    printf("components: alpha=%d beta=%d\n", alphaResult, betaResult);

    Alpha___strata_context_destroy(alpha);
    Beta___strata_context_destroy(beta);

    return 0;
}
