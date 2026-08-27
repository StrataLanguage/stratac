#include "Util.h"
#include "Test.h"
#include "AST/AST.h"
#include "strata/strata.h"

#include <stdio.h>
#include <string.h>

#if STRATA_TEST_HAS_LLVM
#include "Codegen/LLVMJit.h"
#include "Codegen/LLVMModuleBuilder.h"
#include <stdint.h>
#endif

/* TypeNameParse must rebuild a structural tree whose every subtree carries
   the canonical spelling of that subtree (first bracket group = outermost
   dimension, `^` binding tighter than `[]`). */
STRATA_TEST(type_name_parse_round_trips)
{
    Arena arena;
    arena_init(&arena, 0);

    struct
    {
        const char* spelling;
        bool dynamic;
        bool fixed;
        long length;
        const char* elemName;
        bool elemIsBox;
    } cases[] = {
        {"int", false, false, -1, NULL, false},
        {"Foo", false, false, -1, NULL, false},
        {"int[]", true, false, -1, "int", false},
        {"int[4]", false, true, 4, "int", false},
        {"int[2][6]", false, true, 2, "int[6]", false},
        {"^Foo", false, false, -1, NULL, false},
        {"^Foo[]", true, false, -1, "^Foo", true},
        {"^Foo[2]", false, true, 2, "^Foo", true},
    };

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
    {
        TypeName t = TypeNameParse(&arena, cases[i].spelling);

        STRATA_CHECK(t.name && strcmp(t.name, cases[i].spelling) == 0);
        STRATA_CHECK_EQ(TypeNameIsDynamicArray(&t), cases[i].dynamic);
        STRATA_CHECK_EQ(TypeNameIsFixedArray(&t), cases[i].fixed);
        STRATA_CHECK_EQ(TypeNameArrayLength(&t), cases[i].length);

        if (cases[i].elemName)
        {
            const TypeName* elem = TypeNameArrayElem(&t);
            STRATA_CHECK(elem != NULL);
            if (elem)
            {
                STRATA_CHECK(strcmp(elem->name, cases[i].elemName) == 0);
                STRATA_CHECK_EQ(TypeNameIsBox(elem), cases[i].elemIsBox);
            }
        }
        else
        {
            STRATA_CHECK(TypeNameArrayElem(&t) == NULL);
        }
    }

    /* Owning-ness is structural: string, ^T and dynamic T[] own; T[N] does not. */
    STRATA_CHECK(TypeNameIsOwning(&(TypeName){.name = (char*)"string"}));
    TypeName fixed = TypeNameParse(&arena, "int[4]");
    STRATA_CHECK(!TypeNameIsOwning(&fixed));    TypeName dyn = TypeNameParse(&arena, "int[]");
    STRATA_CHECK(TypeNameIsOwning(&dyn));
    TypeName box = TypeNameParse(&arena, "^Foo");
    STRATA_CHECK(TypeNameIsOwning(&box));
    STRATA_CHECK(TypeNameBoxInner(&box) && strcmp(TypeNameBoxInner(&box)->name, "Foo") == 0);

    /* `T[]?` round-trips as an optional wrapping a dynamic array - distinct
       from `T?[]`, which is an array of optionals. */
    TypeName optArr = TypeNameParse(&arena, "int[]?");
    STRATA_CHECK(optArr.name && strcmp(optArr.name, "int[]?") == 0);
    STRATA_CHECK(optArr.isOptional);
    STRATA_CHECK(optArr.inner && TypeNameIsDynamicArray(optArr.inner));
    STRATA_CHECK(optArr.inner && strcmp(optArr.inner->name, "int[]") == 0);
    STRATA_CHECK(TypeNameIsOwning(&optArr));

    TypeName arrOfOpt = TypeNameParse(&arena, "int?[]");
    STRATA_CHECK(!arrOfOpt.isOptional);
    STRATA_CHECK(TypeNameIsDynamicArray(&arrOfOpt));
    STRATA_CHECK(TypeNameArrayElem(&arrOfOpt) != NULL);
    STRATA_CHECK(TypeNameArrayElem(&arrOfOpt)->isOptional);

    arena_free(&arena);
}

static StrataJit* CompileArr(const char* src, const char** err)
{
    StrataCompiler* c = strataCompilerCreate();
    StrataJit* jit = strataJitCompileString(c, src, "arr", err);
    strataCompilerDestroy(c);
    return jit;
}

STRATA_TEST(array_literal_and_index)
{
    const char* err = NULL;
    StrataJit* jit = CompileArr(
        "int entry() {\n"
        "  int[] a = {1, 2, 3, 4};\n"
        "  return a[0] + a[1] + a[2] + a[3];\n"   /* 1+2+3+4 = 10 */
        "}\n",
        &err);

    STRATA_CHECK(jit != NULL);
    if (!jit)
    {
        printf("  JIT failed: %s\n", err ? err : "(none)");
        strataFree((char*)err);
        return;
    }

    int (*entry)(void) = (int (*)(void))strataJitGetFunction(jit, "entry");
    STRATA_CHECK(entry != NULL);
    if (entry)
    {
        STRATA_CHECK_EQ(entry(), 10);
    }

    strataJitDestroy(jit);
}

STRATA_TEST(array_of_strings_iterates_and_drops)
{
    const char* err = NULL;
    StrataJit* jit = CompileArr(
        "int entry() {\n"
        "  string[] names = {\"alpha\", \"beta\", \"gamma\"};\n"
        "  int n = 0;\n"
        "  for (ulong i = 0; i < names.length; i = i + 1) {\n"
        "    n = n + 1;\n"
        "  }\n"
        "  return n;\n"                          /* 3; freeing 3 strings must not crash */
        "}\n",
        &err);

    STRATA_CHECK(jit != NULL);
    if (!jit)
    {
        printf("  JIT failed: %s\n", err ? err : "(none)");
        strataFree((char*)err);
        return;
    }

    int (*entry)(void) = (int (*)(void))strataJitGetFunction(jit, "entry");
    STRATA_CHECK(entry != NULL);
    if (entry)
    {
        STRATA_CHECK_EQ(entry(), 3);
    }

    strataJitDestroy(jit);
}

STRATA_TEST(array_ref_param_borrows)
{
    const char* err = NULL;
    StrataJit* jit = CompileArr(
        "int first(ref int[] a) {\n"
        "  return a[0];\n"
        "}\n"
        "int entry() {\n"
        "  int[] a = {42, 7};\n"
        "  int f = first(a);\n"                  /* borrow: a still usable */
        "  return f + (int)a.length;\n"          /* 42 + 2 = 44 */
        "}\n",
        &err);

    STRATA_CHECK(jit != NULL);
    if (!jit)
    {
        printf("  JIT failed: %s\n", err ? err : "(none)");
        strataFree((char*)err);
        return;
    }

    int (*entry)(void) = (int (*)(void))strataJitGetFunction(jit, "entry");
    STRATA_CHECK(entry != NULL);
    if (entry)
    {
        STRATA_CHECK_EQ(entry(), 44);
    }

    strataJitDestroy(jit);
}

STRATA_TEST(array_rebind_replaces_contents)
{
    const char* err = NULL;
    StrataJit* jit = CompileArr(
        "int entry() {\n"
        "  int[] a = {1, 2, 3};\n"
        "  a = {10, 20};\n"                      /* rebind: free old, take new */
        "  return a[0] + a[1] + (int)a.length;\n" /* 10 + 20 + 2 = 32 */
        "}\n",
        &err);

    STRATA_CHECK(jit != NULL);
    if (!jit)
    {
        printf("  JIT failed: %s\n", err ? err : "(none)");
        strataFree((char*)err);
        return;
    }

    int (*entry)(void) = (int (*)(void))strataJitGetFunction(jit, "entry");
    STRATA_CHECK(entry != NULL);
    if (entry)
    {
        STRATA_CHECK_EQ(entry(), 32);
    }

    strataJitDestroy(jit);
}

STRATA_TEST(array_move_value_is_owned_by_dest)
{
    const char* err = NULL;
    StrataJit* jit = CompileArr(
        "int entry() {\n"
        "  int[] a = {5, 6, 7};\n"
        "  int[] b = a;\n"                       /* move: b owns the buffer now */
        "  return b[2];\n"                       /* 7 (reading a after this is a sema error) */
        "}\n",
        &err);

    STRATA_CHECK(jit != NULL);
    if (!jit)
    {
        printf("  JIT failed: %s\n", err ? err : "(none)");
        strataFree((char*)err);
        return;
    }

    int (*entry)(void) = (int (*)(void))strataJitGetFunction(jit, "entry");
    STRATA_CHECK(entry != NULL);
    if (entry)
    {
        STRATA_CHECK_EQ(entry(), 7);
    }

    strataJitDestroy(jit);
}

STRATA_TEST(array_in_loop_does_not_crash)
{
    /* A fresh array per iteration is freed each time (block scope). */
    const char* err = NULL;
    StrataJit* jit = CompileArr(
        "int entry() {\n"
        "  int sum = 0;\n"
        "  for (int i = 0; i < 50; i = i + 1) {\n"
        "    int[] a = {i, i, i};\n"
        "    sum = sum + a[0] + a[1] + a[2];\n"
        "  }\n"
        "  return sum;\n"                        /* 3 * (0+1+..+49) = 3*1225 = 3675 */
        "}\n",
        &err);

    STRATA_CHECK(jit != NULL);
    if (!jit)
    {
        printf("  JIT failed: %s\n", err ? err : "(none)");
        strataFree((char*)err);
        return;
    }

    int (*entry)(void) = (int (*)(void))strataJitGetFunction(jit, "entry");
    STRATA_CHECK(entry != NULL);
    if (entry)
    {
        STRATA_CHECK_EQ(entry(), 3675);
    }

    strataJitDestroy(jit);
}

STRATA_TEST(array_returned_from_function)
{
    const char* err = NULL;
    StrataJit* jit = CompileArr(
        "int[] three() {\n"
        "  return {10, 20, 30};\n"
        "}\n"
        "int entry() {\n"
        "  int[] a = three();\n"
        "  return a[0] + a[1] + a[2];\n"         /* 60 */
        "}\n",
        &err);

    STRATA_CHECK(jit != NULL);
    if (!jit)
    {
        printf("  JIT failed: %s\n", err ? err : "(none)");
        strataFree((char*)err);
        return;
    }

    int (*entry)(void) = (int (*)(void))strataJitGetFunction(jit, "entry");
    STRATA_CHECK(entry != NULL);
    if (entry)
    {
        STRATA_CHECK_EQ(entry(), 60);
    }

    strataJitDestroy(jit);
}

STRATA_TEST(array_of_structs_sums_field)
{
    const char* err = NULL;
    StrataJit* jit = CompileArr(
        "struct Pt { int x; int y; };\n"
        "int entry() {\n"
        "  Pt[] pts = { Pt{ .x = 1, .y = 2 }, Pt{ .x = 3, .y = 4 } };\n"
        "  int total = 0;\n"
        "  for (ulong i = 0; i < pts.length; i = i + 1) {\n"
        "    total = total + pts[i].x + pts[i].y;\n"
        "  }\n"
        "  return total;\n"                       /* (1+2) + (3+4) = 10 */
        "}\n",
        &err);

    STRATA_CHECK(jit != NULL);
    if (!jit)
    {
        printf("  JIT failed: %s\n", err ? err : "(none)");
        strataFree((char*)err);
        return;
    }

    int (*entry)(void) = (int (*)(void))strataJitGetFunction(jit, "entry");
    STRATA_CHECK(entry != NULL);
    if (entry)
    {
        STRATA_CHECK_EQ(entry(), 10);
    }

    strataJitDestroy(jit);
}

STRATA_TEST(array_use_after_move_is_error)
{
    const char* err = NULL;
    StrataJit* jit = CompileArr(
        "void take(int[] a) {}\n"
        "int entry() {\n"
        "  int[] a = {1,2,3};\n"
        "  take(a);\n"
        "  return (int)a.length;\n"              /* used after move into take() */
        "}\n",
        &err);

    STRATA_CHECK(jit == NULL);
    STRATA_CHECK(err != NULL);
    strataFree((char*)err);
}

STRATA_TEST(array_push_rejects_non_array_argument)
{
    const char* err = NULL;
    StrataJit* jit = CompileArr(
        "int entry() {\n"
        "  int x = 5;\n"
        "  array_push(x, 1);\n"              /* x is not an array */
        "  return x;\n"
        "}\n",
        &err);

    STRATA_CHECK(jit == NULL);
    STRATA_CHECK(err != NULL);
    strataFree((char*)err);
}

STRATA_TEST(array_push_rejects_wrong_arg_count)
{
    const char* err = NULL;
    StrataJit* jit = CompileArr(
        "int entry() {\n"
        "  int[] a = {1, 2, 3};\n"
        "  array_push(a);\n"                 /* missing value */
        "  return (int)a.length;\n"
        "}\n",
        &err);

    STRATA_CHECK(jit == NULL);
    STRATA_CHECK(err != NULL);
    strataFree((char*)err);
}

STRATA_TEST(array_push_owning_value_from_index_is_error)
{
    /* Pushing an owning value read out of an array element would duplicate
       ownership (double-free); it must be rejected. */
    const char* err = NULL;
    StrataJit* jit = CompileArr(
        "int entry() {\n"
        "  string[] s = {\"a\", \"b\"};\n"
        "  array_push(s, s[0]);\n"           /* borrowed owning value */
        "  return (int)s.length;\n"
        "}\n",
        &err);

    STRATA_CHECK(jit == NULL);
    STRATA_CHECK(err != NULL);
    strataFree((char*)err);
}

STRATA_TEST(array_of_owning_struct_must_be_boxed)
{
    /* A struct holding an owning field is itself owning; holding it by value
       as an array element would leak its fields, so it must be boxed. */
    const char* err = NULL;
    StrataJit* jit = CompileArr(
        "struct S { string s; };\n"
        "int entry() {\n"
        "  S[] arr = { S{.s = \"a\"} };\n"   /* illegal: use ^S[] */
        "  return (int)arr.length;\n"
        "}\n",
        &err);

    STRATA_CHECK(jit == NULL);
    STRATA_CHECK(err != NULL);
    strataFree((char*)err);
}

STRATA_TEST(array_uninitialized_decl_allowed_empty)
{
    const char* err = NULL;
    StrataJit* jit = CompileArr(
        "int entry() {\n"
        "  int[] a;\n"
        "  return (int)a.length;\n"              /* 0 */
        "}\n",
        &err);

    STRATA_CHECK(jit != NULL);
    if (!jit)
    {
        printf("  JIT failed: %s\n", err ? err : "(none)");
        strataFree((char*)err);
        return;
    }

    int (*entry)(void) = (int (*)(void))strataJitGetFunction(jit, "entry");
    STRATA_CHECK(entry != NULL);
    if (entry)
    {
        STRATA_CHECK_EQ(entry(), 0);
    }

    strataJitDestroy(jit);
}

/* A dynamic T[] struct field may be omitted from a struct literal: the
   zero-fill IS the canonical empty {null, 0} array, exactly like an
   uninitialized local. (There is no `T[]?` spelling to suggest - a box
   never wraps an array.) */
STRATA_TEST(array_field_omitted_starts_empty)
{
    const char* err = NULL;
    StrataJit* jit = CompileArr(
        "struct S { int[] ints; int other; };\n"
        "int entry() {\n"
        "  ^S s = S { .other = 1 };\n"
        "  int before = (int)s.ints.length;     /* 0 */\n"
        "  array_push(s.ints, 7);\n"
        "  array_push(s.ints, 8);\n"
        "  int sum = s.ints[0] + s.ints[1];     /* 15 */\n"
        "  return before * 1000 + sum + s.other * 10;\n"
        "}\n",
        &err);

    STRATA_CHECK(jit != NULL);
    if (!jit)
    {
        printf("  JIT failed: %s\n", err ? err : "(none)");
        strataFree((char*)err);
        return;
    }

    int (*entry)(void) = (int (*)(void))strataJitGetFunction(jit, "entry");
    STRATA_CHECK(entry != NULL);
    if (entry)
    {
        STRATA_CHECK_EQ(entry(), 25);
    }

    strataJitDestroy(jit);
}

/* The string flavor: an omitted string[] field is empty, growable, and the
   pushed string is readable via .length indexing arithmetic. */
STRATA_TEST(string_array_field_omitted_starts_empty)
{
    const char* err = NULL;
    StrataJit* jit = CompileArr(
        "struct S { string[] words; };\n"
        "int entry() {\n"
        "  ^S s = S { };\n"
        "  int before = (int)s.words.length;    /* 0 */\n"
        "  array_push(s.words, \"abc\");\n"
        "  array_push(s.words, \"de\");\n"
        "  return before * 100 + (int)s.words.length * 10;\n"   /* 20 */
        "}\n",
        &err);

    STRATA_CHECK(jit != NULL);
    if (!jit)
    {
        printf("  JIT failed: %s\n", err ? err : "(none)");
        strataFree((char*)err);
        return;
    }

    int (*entry)(void) = (int (*)(void))strataJitGetFunction(jit, "entry");
    STRATA_CHECK(entry != NULL);
    if (entry)
    {
        STRATA_CHECK_EQ(entry(), 20);
    }

    strataJitDestroy(jit);
}

/* Owning NON-array fields still must be initialized; only the suggestion
   is spelled for real optional types now that arrays are exempt. */
STRATA_TEST(box_field_omitted_still_an_error)
{
    Arena arena; arena_init(&arena, 0);
    DiagnosticEngine diag; DiagnosticEngineInit(&diag);
    ParseAndResolve(
        "struct Inner { int x; };\n"
        "struct S { ^Inner inner; };\n"
        "int entry() { ^S s = S { }; return 0; }\n",
        &diag, &arena);
    STRATA_CHECK(DiagHasErrors(&diag));

    SourceManager sm; SourceManagerInit(&sm);
    char* d = DiagFormat(&diag, &sm, 1, &arena);
    STRATA_CHECK(strstr(d, "must be initialized") != NULL);
    STRATA_CHECK(strstr(d, "'Inner?'") != NULL);

    DiagnosticEngineFree(&diag);
    arena_free(&arena);
}

/* A braced list is valid in expression position: positional constructor
   args accept `{}` (empty array) and `{1, 2, ...}` (element type inferred
   from the field). */
STRATA_TEST(braced_literal_as_constructor_argument)
{
    const char* err = NULL;
    StrataJit* jit = CompileArr(
        "struct FooBar { string str; int[] ints; };\n"
        "int entry() {\n"
        "  ^FooBar a = FooBar(\"x\", {});\n"
        "  ^FooBar b = FooBar(\"y\", {1, 2, 3});\n"
        "  int sum = 0;\n"
        "  for (uint i = 0; i < b.ints.length; i++) { sum += b.ints[i]; }\n"
        "  return sum * 10 + (int)a.ints.length;\n"     /* 60 */
        "}\n",
        &err);

    STRATA_CHECK(jit != NULL);
    if (!jit)
    {
        printf("  JIT failed: %s\n", err ? err : "(none)");
        strataFree((char*)err);
        return;
    }

    int (*entry)(void) = (int (*)(void))strataJitGetFunction(jit, "entry");
    STRATA_CHECK(entry != NULL);
    if (entry)
    {
        STRATA_CHECK_EQ(entry(), 60);
    }

    strataJitDestroy(jit);
}

/* Braced STRUCT literals in expression position: `{}` and `{ .f = ... }`
   against struct-shaped targets - call args, assignments, nested fields,
   returns through plain/box types. Sema fills the type from context. */
STRATA_TEST(braced_struct_literals_in_expression_position){
    const char* err = NULL;
    StrataJit* jit = CompileArr(
        "struct Vec3 { float x; float y; float z; };\n"
        "struct Inner { int a; };\n"
        "struct Outer { Inner in; int tag; };\n"
        "int take_vec(Vec3 v) { return (int)(v.x + v.y + v.z); }\n"
        "Vec3 zero() { return {}; }\n"
        "^Inner boxed_empty() { return {}; }\n"
        "int entry() {\n"
        "  Vec3 v = {};\n"
        "  ^Outer o = {};\n"
        "  o = {};\n"                                /* re-assign braces into the box */
        "  int a = take_vec({});\n"                  /* 0 */
        "  int b = take_vec({1, 2, 3});\n"           /* 6: positional struct */
        "  ^Outer p = Outer({ .a = 7 }, 1);\n"       /* designator braced ctor arg */
        "  Vec3 z = zero();\n"
        "  ^Inner be = boxed_empty();\n"
        "  return a + b + p.in.a + (int)z.x + be.a + o.tag;\n"   /* 6 + 7 = 13 */
        "}\n",
        &err);

    STRATA_CHECK(jit != NULL);
    if (!jit)
    {
        printf("  JIT failed: %s\n", err ? err : "(none)");
        strataFree((char*)err);
        return;
    }

    int (*entry)(void) = (int (*)(void))strataJitGetFunction(jit, "entry");
    STRATA_CHECK(entry != NULL);
    if (entry)
    {
        STRATA_CHECK_EQ(entry(), 13);
    }

    strataJitDestroy(jit);
}

/* `{}` against a `T?` field constructs the boxed T (non-empty), the same
   as a plain `T` field - it does NOT leave the optional empty/null. */
STRATA_TEST(braced_empty_against_optional_field_constructs)
{
    const char* err = NULL;
    StrataJit* jit = CompileArr(
        "struct Node { int v; Node? next; };\n"
        "int entry() {\n"
        "  ^Node a = Node(1, {});\n"              /* next = boxed default Node */
        "  if (a.next?)\n"
        "  {\n"
        "    return 10 + a.next.v;\n"              /* 10 */
        "  }\n"
        "  return 1;\n"
        "}\n",
        &err);

    STRATA_CHECK(jit != NULL);
    if (!jit)
    {
        printf("  JIT failed: %s\n", err ? err : "(none)");
        strataFree((char*)err);
        return;
    }

    int (*entry)(void) = (int (*)(void))strataJitGetFunction(jit, "entry");
    STRATA_CHECK(entry != NULL);
    if (entry)
    {
        STRATA_CHECK_EQ(entry(), 10);
    }

    strataJitDestroy(jit);
}

/* ---- Pushing a narrowed `T?` into a `^T[]` --------------------------------
   `if (x.next?) { array_push(arr, x.next); }` moves the proven box into
   the element slot: the source optional is left EMPTY (legal), the parent
   box is NOT poisoned, and the move works through refs and box globals. */
STRATA_TEST(push_narrowed_optional_into_box_array)
{
    const char* err = NULL;
    StrataJit* jit = CompileArr(
        "struct Node { int v; Node? next; };\n"
        "int drain(ref ^Node[] sink, ^Node src)\n"
        "{\n"
        "  if (src.next?) { array_push(sink, src.next); }\n"   /* through a ref */
        "  return 0;\n"
        "}\n"
        "int entry() {\n"
        "  ^Node[] arr;\n"
        "  ^Node a = Node(1, {});\n"                /* next constructed, non-empty */
        "  if (a.next?)\n"
        "  {\n"
        "    array_push(arr, a.next);\n"             /* the move */
        "  }\n"
        "  int r = (int)arr.length * 10;\n"          /* 10 */
        "  if (a.next?) { r += 5; } else { r += 1; }\n"   /* emptied -> +1 -> 11 */
        "  ^Node[] more;\n"
        "  array_push(more, a);\n"                   /* parent NOT poisoned */
        "  r += (int)more.length * 100;\n"           /* +100 -> 111 */ 
        "  ^Node b = Node(7, {});\n"
        "  drain(arr, b);\n"                         /* pushes b's nested next (v == 0) */
        "  r += (int)arr.length;\n"                  /* 2 -> 113 */
        "  return r + arr[1].v;\n"                   /* +0 -> 113 */
        "}\n",
        &err);

    STRATA_CHECK(jit != NULL);
    if (!jit)
    {
        printf("  JIT failed: %s\n", err ? err : "(none)");
        strataFree((char*)err);
        return;
    }

    int (*entry)(void) = (int (*)(void))strataJitGetFunction(jit, "entry");
    STRATA_CHECK(entry != NULL);
    if (entry)
    {
        STRATA_CHECK_EQ(entry(), 113);
    }

    strataJitDestroy(jit);
}

STRATA_TEST(push_unnarrowed_optional_into_box_array_is_error)
{
    Arena arena; arena_init(&arena, 0);
    DiagnosticEngine diag; DiagnosticEngineInit(&diag);
    ParseAndResolve(
        "struct Node { int v; Node? next; };\n"
        "int entry() {\n"
        "  ^Node[] arr;\n"
        "  ^Node a = Node(1, {});\n"
        "  array_push(arr, a.next);\n"      /* no `if (a.next?)` guard */
        "  return 0;\n"
        "}\n",
        &diag, &arena);
    STRATA_CHECK(DiagHasErrors(&diag));

    SourceManager sm; SourceManagerInit(&sm);
    char* d = DiagFormat(&diag, &sm, 1, &arena);
    STRATA_CHECK(strstr(d, "cannot push a value of type 'Node?'") != NULL);

    DiagnosticEngineFree(&diag);
    arena_free(&arena);
}

/* ---- 2D arrays -------------------------------------------------------------
   Dynamic `T[][]` literals (the parser types only the outermost literal;
   sema propagates the row element type) and fixed `T[N][M]` struct fields
   (braced initializers are FLAT C-style lists - nested rows diagnose). */

STRATA_TEST(two_d_dynamic_array_literal_runs)
{
    const char* err = NULL;
    StrataJit* jit = CompileArr(
        "int entry() {\n"
        "  int[][] grid = { {1, 2}, {3, 4} };\n"
        "  int sum = 0;\n"
        "  for (uint i = 0; i < grid.length; i++)\n"
        "  {\n"
        "    for (uint j = 0; j < grid[i].length; j++)\n"
        "    {\n"
        "      sum += grid[i][j];\n"
        "    }\n"
        "  }\n"
        "  grid[1][0] = 30;\n"                    /* mutate a cell */
        "  return sum * 100 + grid[1][0] + (int)grid.length + (int)grid[0].length;\n"   /* 1000+30+2+2 */
        "}\n",
        &err);

    STRATA_CHECK(jit != NULL);
    if (!jit)
    {
        printf("  JIT failed: %s\n", err ? err : "(none)");
        strataFree((char*)err);
        return;
    }

    int (*entry)(void) = (int (*)(void))strataJitGetFunction(jit, "entry");
    STRATA_CHECK(entry != NULL);
    if (entry)
    {
        STRATA_CHECK_EQ(entry(), 1034);
    }

    strataJitDestroy(jit);
}

STRATA_TEST(two_d_fixed_array_nested_rows_required_and_run)
{
    /* Multidimensional fixed fields take nested rows - one brace level per
       dimension, rows may be short (missing elements zero). */
    const char* err = NULL;
    StrataJit* jit = CompileArr(
        "struct Grid { int[2][3] cells; };\n"
        "int entry() {\n"
        "  Grid a = Grid { .cells = { {10, 20, 30}, {40, 50, 60} } };\n"
        "  Grid b = Grid { .cells = { {1, 2} } };\n"          /* short row + missing row */
        "  int sum = a.cells[0][0] + a.cells[1][2] + b.cells[0][1] + b.cells[0][2] + b.cells[1][0];\n"
        "  return sum + (int)a.cells.length;\n"      /* 10+60+2+0+0 + 2 */
        "}\n",
        &err);

    STRATA_CHECK(jit != NULL);
    if (!jit)
    {
        printf("  JIT failed: %s\n", err ? err : "(none)");
        strataFree((char*)err);
        return;
    }

    int (*entry)(void) = (int (*)(void))strataJitGetFunction(jit, "entry");
    STRATA_CHECK(entry != NULL);
    if (entry)
    {
        STRATA_CHECK_EQ(entry(), 74);
    }

    strataJitDestroy(jit);
}

STRATA_TEST(two_d_fixed_array_flat_list_for_multidim_is_error)
{
    /* A flat list for a multidimensional field is a shape violation - the
       initializer must mirror the dimensions. */
    Arena arena; arena_init(&arena, 0);
    DiagnosticEngine diag; DiagnosticEngineInit(&diag);
    ParseAndResolve(
        "struct Grid { int[2][3] cells; };\n"
        "int entry() {\n"
        "  Grid g = Grid { .cells = {10, 20, 30, 40, 50, 60} };\n"
        "  return 0;\n"
        "}\n",
        &diag, &arena);
    STRATA_CHECK(DiagHasErrors(&diag));

    SourceManager sm; SourceManagerInit(&sm);
    char* d = DiagFormat(&diag, &sm, 1, &arena);
    STRATA_CHECK(strstr(d, "requires nested rows") != NULL);

    DiagnosticEngineFree(&diag);
    arena_free(&arena);
}

STRATA_TEST(one_d_fixed_array_nested_row_is_error)
{
    /* The mirror rule: a single-dimension field takes a flat list only. */
    Arena arena; arena_init(&arena, 0);
    DiagnosticEngine diag; DiagnosticEngineInit(&diag);
    ParseAndResolve(
        "struct Buf { byte[4] data; };\n"
        "int entry() {\n"
        "  Buf b = Buf { .data = { {1, 2}, {3, 4} } };\n"
        "  return 0;\n"
        "}\n",
        &diag, &arena);
    STRATA_CHECK(DiagHasErrors(&diag));

    SourceManager sm; SourceManagerInit(&sm);
    char* d = DiagFormat(&diag, &sm, 1, &arena);
    STRATA_CHECK(strstr(d, "single dimension - write its elements as a flat list") != NULL);

    DiagnosticEngineFree(&diag);
    arena_free(&arena);
}

/* ---- Multidimensional fixed arrays of STRUCTS -----------------------------
   Brace-init struct elements in nested rows - typed literals, bare braces,
   and positional calls - must survive the row placement without confusing
   the parser or sema, including short rows leaving holes. */

STRATA_TEST(multidim_fixed_array_struct_elements_typed_literals)
{
    const char* err = NULL;
    StrataJit* jit = CompileArr(
        "struct Cell { int v; int w; };\n"
        "struct Grid { Cell[2][3] cells; };\n"
        "int entry() {\n"
        "  Grid g = Grid { .cells = { { Cell { .v = 1, .w = 10 }, Cell { .v = 2, .w = 20 }, Cell { .v = 3, .w = 30 } },\n"
        "                          { Cell { .v = 4, .w = 40 }, Cell { .v = 5, .w = 50 }, Cell { .v = 6, .w = 60 } } } };\n"
        "  int sum = g.cells[0][0].v + g.cells[0][2].v + g.cells[1][1].v;\n"     /* 1+3+5 */
        "  int wsum = g.cells[0][1].w + g.cells[1][2].w;\n"                     /* 20+60 */
        "  return sum * 100 + wsum + (int)g.cells.length;\n"                    /* 900+80+2 */
        "}\n",
        &err);

    STRATA_CHECK(jit != NULL);
    if (!jit)
    {
        printf("  JIT failed: %s\n", err ? err : "(none)");
        strataFree((char*)err);
        return;
    }

    int (*entry)(void) = (int (*)(void))strataJitGetFunction(jit, "entry");
    STRATA_CHECK(entry != NULL);
    if (entry)
    {
        STRATA_CHECK_EQ(entry(), 982);
    }

    strataJitDestroy(jit);
}

STRATA_TEST(multidim_fixed_array_struct_elements_bare_braces)
{
    /* Bare-brace struct elements deep inside row literals carry no type
       name from the parser - sema fills them from the leaf type. Short
       rows leave holes: `{4}` belongs to cells[1][0], NOT cells[0][2]
       (row-major placement, C semantics). */
    const char* err = NULL;
    StrataJit* jit = CompileArr(
        "struct Cell { int v; };\n"
        "struct Grid { Cell[2][3] cells; };\n"
        "int entry() {\n"
        "  Grid g = Grid { .cells = { { { .v = 1 }, { .v = 2 } },\n"
        "                          { { .v = 4 } } } };\n"
        "  int a = g.cells[0][0].v;\n"      /* 1 */
        "  int b = g.cells[0][1].v;\n"      /* 2 */
        "  int hole = g.cells[0][2].v;\n"   /* short row left a hole -> 0 */
        "  int c = g.cells[1][0].v;\n"      /* 4: next row starts at ITS offset */
        "  return a * 1000 + b * 100 + hole * 10 + c;\n"   /* 1204 */
        "}\n",
        &err);

    STRATA_CHECK(jit != NULL);
    if (!jit)
    {
        printf("  JIT failed: %s\n", err ? err : "(none)");
        strataFree((char*)err);
        return;
    }

    int (*entry)(void) = (int (*)(void))strataJitGetFunction(jit, "entry");
    STRATA_CHECK(entry != NULL);
    if (entry)
    {
        STRATA_CHECK_EQ(entry(), 1204);
    }

    strataJitDestroy(jit);
}

STRATA_TEST(multidim_fixed_array_struct_elements_mixed_forms)
{
    /* Typed literals, bare braces, and positional ctor calls mix freely
       within the same initializer. */
    const char* err = NULL;
    StrataJit* jit = CompileArr(
        "struct Cell { int v; };\n"
        "struct Grid { Cell[2][2] cells; };\n"
        "int entry() {\n"
        "  Grid g = Grid { .cells = { { Cell { .v = 1 }, { .v = 2 } },\n"
        "                          { Cell(3), { .v = 4 } } } };\n"
        "  return g.cells[0][0].v + g.cells[0][1].v + g.cells[1][0].v + g.cells[1][1].v;\n"   /* 10 */
        "}\n",
        &err);

    STRATA_CHECK(jit != NULL);
    if (!jit)
    {
        printf("  JIT failed: %s\n", err ? err : "(none)");
        strataFree((char*)err);
        return;
    }

    int (*entry)(void) = (int (*)(void))strataJitGetFunction(jit, "entry");
    STRATA_CHECK(entry != NULL);
    if (entry)
    {
        STRATA_CHECK_EQ(entry(), 10);
    }

    strataJitDestroy(jit);
}

/* ---- Optional dynamic arrays (`T[]?`) ------------------------------------
   A `T[]?` field/local is an optional array: it shares the fat {ptr, len}
   representation with `T[]` (empty = {null, 0}), but reading it requires
   narrowing - unlike a plain T[], which is always usable. */

STRATA_TEST(optional_array_field_narrows_and_runs)
{
    const char* err = NULL;
    StrataJit* jit = CompileArr(
        "struct Bag { int[]? items; string label; };\n"
        "int entry() {\n"
        "  ^Bag b = Bag { .label = \"empty\" };\n"
        "  int unset = 0;\n"
        "  if (b.items?) { } else { unset = 1; }\n"
        "  ^Bag o = Bag { .label = \"full\" };\n"
        "  o.items = {1, 2, 3};\n"                    /* rebind the optional array */
        "  int sum = 0;\n"
        "  if (o.items?)\n"
        "  {\n"
        "    array_push(o.items, 4);\n"
        "    for (uint i = 0; i < o.items.length; i++) { sum += o.items[i]; }\n"
        "  }\n"
        "  return unset * 1000 + sum;\n"              /* 1010 */
        "}\n",
        &err);

    STRATA_CHECK(jit != NULL);
    if (!jit)
    {
        printf("  JIT failed: %s\n", err ? err : "(none)");
        strataFree((char*)err);
        return;
    }

    int (*entry)(void) = (int (*)(void))strataJitGetFunction(jit, "entry");
    STRATA_CHECK(entry != NULL);
    if (entry)
    {
        STRATA_CHECK_EQ(entry(), 1010);
    }

    strataJitDestroy(jit);
}

STRATA_TEST(optional_array_use_without_narrowing_is_error)
{
    Arena arena; arena_init(&arena, 0);
    DiagnosticEngine diag; DiagnosticEngineInit(&diag);
    ParseAndResolve(
        "struct Bag { int[]? items; };\n"
        "int entry() {\n"
        "  ^Bag b = Bag { };\n"
        "  int x = b.items[0];\n"
        "  array_push(b.items, 1);\n"
        "  int n = (int)b.items.length;\n"
        "  return x + n;\n"
        "}\n",
        &diag, &arena);
    STRATA_CHECK(DiagHasErrors(&diag));

    SourceManager sm; SourceManagerInit(&sm);
    char* d = DiagFormat(&diag, &sm, 1, &arena);
    STRATA_CHECK(strstr(d, "'b.items' has not been blessed") != NULL);

    DiagnosticEngineFree(&diag);
    arena_free(&arena);
}

STRATA_TEST(fixed_array_cannot_be_optional)
{
    Arena arena; arena_init(&arena, 0);
    DiagnosticEngine diag; DiagnosticEngineInit(&diag);
    ParseAndResolve(
        "struct S { int[2] ints; };\n"
        "int entry() { return 0; }\n",
        &diag, &arena);
    STRATA_CHECK(!DiagHasErrors(&diag));   /* sanity: plain fixed array still fine */

    DiagnosticEngineFree(&diag);
    arena_free(&arena);

    DiagnosticEngine diag2; DiagnosticEngineInit(&diag2);
    ParseAndResolve(
        "struct S { int[2]? ints; };\n"
        "int entry() { return 0; }\n",
        &diag2, &arena);
    STRATA_CHECK(DiagHasErrors(&diag2));

    SourceManager sm2; SourceManagerInit(&sm2);
    char* d2 = DiagFormat(&diag2, &sm2, 1, &arena);
    STRATA_CHECK(strstr(d2, "cannot be optional") != NULL);

    DiagnosticEngineFree(&diag2);
    arena_free(&arena);
}

STRATA_TEST(global_array_uninitialized_is_empty)
{
    const char* err = NULL;
    StrataJit* jit = CompileArr(
        "int[] g;\n"
        "int entry() {\n"
        "  return (int)g.length;\n"            /* 0 */
        "}\n",
        &err);

    STRATA_CHECK(jit != NULL);
    if (!jit)
    {
        printf("  JIT failed: %s\n", err ? err : "(none)");
        strataFree((char*)err);
        return;
    }

    int (*entry)(void) = (int (*)(void))strataJitGetFunction(jit, "entry");
    STRATA_CHECK(entry != NULL);
    if (entry)
    {
        STRATA_CHECK_EQ(entry(), 0);
    }

    strataJitDestroy(jit);
}

STRATA_TEST(global_array_initializer_can_be_read)
{
    const char* err = NULL;
    StrataJit* jit = CompileArr(
        "int[] g = {1, 2, 3};\n"
        "int entry() {\n"
        "  return g[0] + g[1] + g[2] + (int)g.length;\n"   /* 1+2+3 + 3 = 9 */
        "}\n",
        &err);

    STRATA_CHECK(jit != NULL);
    if (!jit)
    {
        printf("  JIT failed: %s\n", err ? err : "(none)");
        strataFree((char*)err);
        return;
    }

    int (*entry)(void) = (int (*)(void))strataJitGetFunction(jit, "entry");
    STRATA_CHECK(entry != NULL);
    if (entry)
    {
        STRATA_CHECK_EQ(entry(), 9);
    }

    strataJitDestroy(jit);
}

STRATA_TEST(global_array_mutations_persist_across_calls)
{
    /* A global outlives each function call: pushes from one call are visible
       to the next. */
    const char* err = NULL;
    StrataJit* jit = CompileArr(
        "int[] g = {1, 2};\n"
        "int add_one() {\n"
        "  array_push(g, 9);\n"
        "  return (int)g.length;\n"
        "}\n"
        "int entry() {\n"
        "  int a = add_one();\n"               /* {1,2,9} -> 3 */
        "  int b = add_one();\n"               /* {1,2,9,9} -> 4 */
        "  return a + b + g[3];\n"             /* 3 + 4 + 9 = 16 */
        "}\n",
        &err);

    STRATA_CHECK(jit != NULL);
    if (!jit)
    {
        printf("  JIT failed: %s\n", err ? err : "(none)");
        strataFree((char*)err);
        return;
    }

    int (*entry)(void) = (int (*)(void))strataJitGetFunction(jit, "entry");
    STRATA_CHECK(entry != NULL);
    if (entry)
    {
        STRATA_CHECK_EQ(entry(), 16);
    }

    strataJitDestroy(jit);
}

STRATA_TEST(global_array_is_mutable_from_functions)
{
    const char* err = NULL;
    StrataJit* jit = CompileArr(
        "int[] g = {1, 2, 3};\n"
        "void setit() {\n"
        "  g[0] = 99;\n"
        "}\n"
        "int entry() {\n"
        "  setit();\n"
        "  return g[0] + g[1] + g[2];\n"        /* 99 + 2 + 3 = 104 */
        "}\n",
        &err);

    STRATA_CHECK(jit != NULL);
    if (!jit)
    {
        printf("  JIT failed: %s\n", err ? err : "(none)");
        strataFree((char*)err);
        return;
    }

    int (*entry)(void) = (int (*)(void))strataJitGetFunction(jit, "entry");
    STRATA_CHECK(entry != NULL);
    if (entry)
    {
        STRATA_CHECK_EQ(entry(), 104);
    }

    strataJitDestroy(jit);
}

STRATA_TEST(global_array_resize)
{
    const char* err = NULL;
    StrataJit* jit = CompileArr(
        "int[] g = {1, 2, 3, 4, 5};\n"
        "int entry() {\n"
        "  array_resize(g, 3);\n"
        "  return g[0] + g[1] + g[2] + (int)g.length;\n"   /* 1+2+3 + 3 = 9 */
        "}\n",
        &err);

    STRATA_CHECK(jit != NULL);
    if (!jit)
    {
        printf("  JIT failed: %s\n", err ? err : "(none)");
        strataFree((char*)err);
        return;
    }

    int (*entry)(void) = (int (*)(void))strataJitGetFunction(jit, "entry");
    STRATA_CHECK(entry != NULL);
    if (entry)
    {
        STRATA_CHECK_EQ(entry(), 9);
    }

    strataJitDestroy(jit);
}

STRATA_TEST(global_array_of_strings_drops_cleanly)
{
    /* The module teardown frees every string in the global array; this must
       not crash or double-free. */
    const char* err = NULL;
    StrataJit* jit = CompileArr(
        "string[] g = {\"alpha\", \"beta\", \"gamma\"};\n"
        "int entry() {\n"
        "  return (int)g.length;\n"            /* 3 */
        "}\n",
        &err);

    STRATA_CHECK(jit != NULL);
    if (!jit)
    {
        printf("  JIT failed: %s\n", err ? err : "(none)");
        strataFree((char*)err);
        return;
    }

    int (*entry)(void) = (int (*)(void))strataJitGetFunction(jit, "entry");
    STRATA_CHECK(entry != NULL);
    if (entry)
    {
        STRATA_CHECK_EQ(entry(), 3);
    }

    strataJitDestroy(jit);
}

STRATA_TEST(global_array_wrong_initializer_type_is_error)
{
    const char* err = NULL;
    StrataJit* jit = CompileArr(
        "int[] g = 5;\n"                       /* not an array initializer */
        "int entry() {\n"
        "  return (int)g.length;\n"
        "}\n",
        &err);

    STRATA_CHECK(jit == NULL);
    STRATA_CHECK(err != NULL);
    strataFree((char*)err);
}

STRATA_TEST(array_length_is_count)
{
    const char* err = NULL;
    StrataJit* jit = CompileArr(
        "ulong entry() {\n"
        "  int[] a = {10, 20, 30, 40, 50};\n"
        "  return a.length;\n"                    /* 5 */
        "}\n",
        &err);

    STRATA_CHECK(jit != NULL);
    if (!jit)
    {
        printf("  JIT failed: %s\n", err ? err : "(none)");
        strataFree((char*)err);
        return;
    }

    unsigned long long (*entry)(void) =
        (unsigned long long (*)(void))strataJitGetFunction(jit, "entry");
    STRATA_CHECK(entry != NULL);
    if (entry)
    {
        STRATA_CHECK_EQ(entry(), 5ULL);
    }

    strataJitDestroy(jit);
}

STRATA_TEST(array_index_is_mutable)
{
    const char* err = NULL;
    StrataJit* jit = CompileArr(
        "int entry() {\n"
        "  int[] a = {1, 2, 3};\n"
        "  a[1] = 20;\n"
        "  return a[1];\n"                        /* 20 */
        "}\n",
        &err);

    STRATA_CHECK(jit != NULL);
    if (!jit)
    {
        printf("  JIT failed: %s\n", err ? err : "(none)");
        strataFree((char*)err);
        return;
    }

    int (*entry)(void) = (int (*)(void))strataJitGetFunction(jit, "entry");
    STRATA_CHECK(entry != NULL);
    if (entry)
    {
        STRATA_CHECK_EQ(entry(), 20);
    }

    strataJitDestroy(jit);
}

STRATA_TEST(array_iterate_and_sum)
{
    const char* err = NULL;
    StrataJit* jit = CompileArr(
        "int entry() {\n"
        "  int[] a = {1, 2, 3, 4};\n"
        "  int sum = 0;\n"
        "  for (ulong i = 0; i < a.length; i = i + 1) {\n"
        "    sum = sum + a[i];\n"
        "  }\n"
        "  return sum;\n"                         /* 10 */
        "}\n",
        &err);

    STRATA_CHECK(jit != NULL);
    if (!jit)
    {
        printf("  JIT failed: %s\n", err ? err : "(none)");
        strataFree((char*)err);
        return;
    }

    int (*entry)(void) = (int (*)(void))strataJitGetFunction(jit, "entry");
    STRATA_CHECK(entry != NULL);
    if (entry)
    {
        STRATA_CHECK_EQ(entry(), 10);
    }

    strataJitDestroy(jit);
}

/* ---- Allocation/free balance for arrays returned from functions ---- */

static int g_arrAllocs = 0;
static int g_arrFrees = 0;

static void* ArrCountAlloc(unsigned long long n)
{
    g_arrAllocs++;
    return malloc((size_t)n);
}

/* Only non-null frees count as real frees: the LLVM backend emits an
   unconditional strata_free(NULL) after a moved-out source is nulled,
   which is harmless but would otherwise skew the balance. */
static void ArrCountFree(void* p)
{
    if (p)
    {
        g_arrFrees++;
    }
    free(p);
}

STRATA_TEST(array_return_cleans_up_memory)
{
    /* One function returns an array to another that hands it back to the
       caller. Every buffer must be freed exactly once: no leak, no
       double-free. Returning an array moves it out of the callee (the
       source slot is zeroed), and the caller owns the buffer until scope
       exit. The C backend must zero the moved source as a whole
       (strata__arr){0} struct, not a bare pointer zero. */
    StrataCompiler* c = strataCompilerCreate();
    strataJitSetAllocFreeFunctions(c, (void*)&ArrCountAlloc, (void*)&ArrCountFree);

    const char* err = NULL;
    StrataJit* jit = strataJitCompileString(c,
        "int[] build(int n) {\n"
        "  int[] a = {1, 2, 3};\n"
        "  array_push(a, n);\n"              /* reallocates: alloc + free */
        "  return a;\n"                      /* moves a out */
        "}\n"
        "int[] relay(int n) {\n"
        "  int[] a = build(n);\n"            /* owns build's array */
        "  return a;\n"                      /* moves it out again */
        "}\n"
        "int sum(ref int[] a) {\n"
        "  int total = 0;\n"
        "  for (ulong i = 0; i < a.length; i = i + 1) { total = total + a[i]; }\n"
        "  return total;\n"
        "}\n"
        "int entry() {\n"
        "  int[] a = relay(4);\n"            /* {1, 2, 3, 4} */
        "  int s = sum(a);\n"                /* 10; a borrowed, still live */
        "  return s + a[0] + a[3];\n"        /* 10 + 1 + 4 = 15 */
        "}\n",
        "arrclean", &err);
    STRATA_CHECK(jit != NULL);
    if (!jit)
    {
        printf("  JIT failed: %s\n", err ? err : "(none)");
        strataFree((char*)err);
        strataCompilerDestroy(c);
        return;
    }

    g_arrAllocs = 0;
    g_arrFrees = 0;

    int (*entry)(void) = (int (*)(void))strataJitGetFunction(jit, "entry");
    STRATA_CHECK(entry != NULL);
    if (entry)
    {
        STRATA_CHECK_EQ(entry(), 15);
        STRATA_CHECK(g_arrAllocs > 0);
        STRATA_CHECK_EQ(g_arrAllocs, g_arrFrees);
    }

    strataJitDestroy(jit);
}

/* `{}` against a `T?` in every position constructs the boxed T (non-empty);
   OMITTING the value entirely is what leaves the optional empty. */
STRATA_TEST(optional_braced_empty_constructs_in_all_positions)
{
    const char* err = NULL;
    StrataJit* jit = CompileArr(
        "struct Node { int v; Node? next; };\n"
        "int entry() {\n"
        "  /* named-literal field */\n"
        "  ^Node a = Node { .v = 1, .next = {} };\n"
        "  int r = 0;\n"
        "  if (a.next?) { r += 1; }\n"
        "  /* assignment into an optional field */\n"
        "  ^Node b = Node { .v = 2 };\n"
        "  b.next = {};\n"
        "  if (b.next?) { r += 10; }\n"
        "  /* plain optional local initialized from braces */\n"
        "  Node? n = {};\n"
        "  if (n?) { r += 100; }\n"
        "  /* omitted trailing ctor arg leaves the optional EMPTY */\n"
        "  ^Node c = Node(3);\n"
        "  if (c.next?) { r += 5000; } else { r += 1000; }\n"
        "  /* reading the constructed default through the narrow */\n"
        "  if (a.next?) { r += a.next.v; }\n"       /* +0 */
        "  return r;\n"                              /* 1111 */
        "}\n",
        &err);

    STRATA_CHECK(jit != NULL);
    if (!jit)
    {
        printf("  JIT failed: %s\n", err ? err : "(none)");
        strataFree((char*)err);
        return;
    }

    int (*entry)(void) = (int (*)(void))strataJitGetFunction(jit, "entry");
    STRATA_CHECK(entry != NULL);
    if (entry)
    {
        STRATA_CHECK_EQ(entry(), 1111);
    }

    strataJitDestroy(jit);
}

/* The recursive-owning-struct shape from samples/arrays.strata: a `{}`
   against the self-referential `T?` field constructs a zeroed cell whose
   owning fields (null string, empty array) drop cleanly, and the
   two-arg-of-three ctor form (trailing arg omitted) stays legal. */
STRATA_TEST(recursive_struct_braced_ctor_sample_shape)
{
    const char* err = NULL;
    StrataJit* jit = CompileArr(
        "struct FooBar { string str; int[] ints; FooBar? next; };\n"
        "int take(^FooBar f) { return (int)f.ints.length; }\n"
        "int entry() {\n"
        "  ^FooBar[] elems;\n"
        "  array_push(elems, FooBar(\"hello\", {}, {}));\n"
        "  array_push(elems, FooBar(\"world\", {}, {}));\n"
        "  array_push(elems, FooBar(\"again\"));\n"   /* next omitted -> empty */
        "  int r = (int)elems.length * 100;\n"        /* 300 */
        "  if (elems[0].next?) { r += 10; }\n"        /* constructed, non-empty */
        "  if (elems[2].next?) { r += 5000; } else { r += 1; }\n"   /* omitted */
        "  return r + take(elems[0]);\n"              /* 311 */
        "}\n",
        &err);

    STRATA_CHECK(jit != NULL);
    if (!jit)
    {
        printf("  JIT failed: %s\n", err ? err : "(none)");
        strataFree((char*)err);
        return;
    }

    int (*entry)(void) = (int (*)(void))strataJitGetFunction(jit, "entry");
    STRATA_CHECK(entry != NULL);
    if (entry)
    {
        STRATA_CHECK_EQ(entry(), 311);
    }

    strataJitDestroy(jit);
}

#if STRATA_TEST_HAS_LLVM
STRATA_TEST(array_return_cleans_up_memory_llvm)
{
    /* The same chained array return through the LLVM backend: the caller
       owns the buffer until scope exit, and every buffer is freed exactly
       once (null frees are not counted). */
    Arena arena;
    arena_init(&arena, 0);
    DiagnosticEngine diag;
    DiagnosticEngineInit(&diag);
    Module* mod = ParseAndResolve(
        "int[] build(int n) {\n"
        "  int[] a = {1, 2, 3};\n"
        "  array_push(a, n);\n"
        "  return a;\n"
        "}\n"
        "int[] relay(int n) {\n"
        "  int[] a = build(n);\n"
        "  return a;\n"
        "}\n"
        "int sum(ref int[] a) {\n"
        "  int total = 0;\n"
        "  for (ulong i = 0; i < a.length; i = i + 1) { total = total + a[i]; }\n"
        "  return total;\n"
        "}\n"
        "int entry() {\n"
        "  int[] a = relay(4);\n"
        "  int s = sum(a);\n"
        "  return s + a[0] + a[3];\n"
        "}\n",
        &diag, &arena);
    STRATA_CHECK(!DiagHasErrors(&diag));
    BuiltModule bm = BuildLlvmModule(mod, &diag, &arena, true, NULL);
    LLVMJit jit;
    LLVMJitInit(&jit);
    LLVMJitSetAllocFree(&jit, (void*)&ArrCountAlloc, (void*)&ArrCountFree);
    char* err = NULL;
    bool ok = LLVMJitLoad(&jit, &bm, &err);
    if (!ok)
    {
        printf("  LLVM JIT failed: %s\n", err ? err : "(none)");
    }
    STRATA_CHECK(ok);

    g_arrAllocs = 0;
    g_arrFrees = 0;

    if (ok)
    {
        int (*entry)(void) = (int (*)(void))(uintptr_t)LLVMJitGetAddress(&jit, "entry");
        STRATA_CHECK(entry != NULL);
        if (entry)
        {
            STRATA_CHECK_EQ(entry(), 15);
            STRATA_CHECK(g_arrAllocs > 0);
            STRATA_CHECK_EQ(g_arrAllocs, g_arrFrees);
        }
    }

    free(err);
    LLVMJitDestroy(&jit);
    BuiltModuleDispose(&bm);
    DiagnosticEngineFree(&diag);
    arena_free(&arena);
}
#endif
