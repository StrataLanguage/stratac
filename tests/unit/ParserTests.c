#include "Util.h"
#include "Test.h"

#include <stdarg.h>
#include <stdint.h>

static ReturnStmt* SingleReturn(Module* m)
{
    STRATA_CHECK(m != NULL);
    if (m->functions.count == 0)
    {
        return NULL;
    }

    FunctionDecl* fn = (FunctionDecl*)VecGet(&m->functions, 0);
    if (!fn->body)
    {
        return NULL;
    }

    Block* block = (Block*)fn->body;
    if (block->statements.count == 0)
    {
        return NULL;
    }

    return (ReturnStmt*)VecGet(&block->statements, 0);
}

STRATA_TEST(parser_simple_function)
{
    Arena arena; arena_init(&arena, 0);
    DiagnosticEngine diag; DiagnosticEngineInit(&diag);
    Module* mod = ParseModule("int f() { return 7; }", &diag, &arena);
    STRATA_CHECK(!DiagHasErrors(&diag));
    STRATA_CHECK(mod->functions.count == 1);

    FunctionDecl* fn = (FunctionDecl*)VecGet(&mod->functions, 0);
    STRATA_CHECK(strcmp(fn->name, "f") == 0);
    STRATA_CHECK(strcmp(fn->returnType.name, "int") == 0);

    ReturnStmt* ret = SingleReturn(mod);
    STRATA_CHECK(ret != NULL);
    STRATA_CHECK(ret->value->kind == NodeIntLiteral);
    STRATA_CHECK_EQ((long)((IntLiteral*)ret->value)->value, 7);

    DiagnosticEngineFree(&diag);
    arena_free(&arena);
}

STRATA_TEST(parser_out_parameter)
{
    Arena arena; arena_init(&arena, 0);
    DiagnosticEngine diag; DiagnosticEngineInit(&diag);
    Module* mod = ParseModule("void g(ref int x) {}", &diag, &arena);
    STRATA_CHECK(!DiagHasErrors(&diag));

    FunctionDecl* fn = (FunctionDecl*)VecGet(&mod->functions, 0);
    STRATA_CHECK_EQ((long)fn->params.count, 1);

    ParamDecl* p = (ParamDecl*)VecGet(&fn->params, 0);
    STRATA_CHECK(p->mod == ModRef);
    STRATA_CHECK(strcmp(p->type.name, "int") == 0);
    STRATA_CHECK(strcmp(p->name, "x") == 0);

    DiagnosticEngineFree(&diag);
    arena_free(&arena);
}

STRATA_TEST(parser_inout_parameter)
{
    Arena arena; arena_init(&arena, 0);
    DiagnosticEngine diag; DiagnosticEngineInit(&diag);
    Module* mod = ParseModule("void h(ref float t) {}", &diag, &arena);
    STRATA_CHECK(!DiagHasErrors(&diag));

    FunctionDecl* fn = (FunctionDecl*)VecGet(&mod->functions, 0);
    ParamDecl* p = (ParamDecl*)VecGet(&fn->params, 0);
    STRATA_CHECK(p->mod == ModRef);
    STRATA_CHECK(strcmp(p->type.name, "float") == 0);

    DiagnosticEngineFree(&diag);
    arena_free(&arena);
}

STRATA_TEST(parser_struct_body_semicolon_optional)
{
    Arena arena; arena_init(&arena, 0);
    DiagnosticEngine diag; DiagnosticEngineInit(&diag);
    Module* mod = ParseModule(
        "struct NoSemi { int x; }\n"
        "struct WithSemi { int y; };\n"
        "extern struct ExtNoSemi { byte[4] tag; }\n"
        "enum NoSemiEnum : int { A, B }\n"
        "enum WithSemiEnum { C, D };\n"
        "int entry() { return 0; }\n",
        &diag, &arena);
    STRATA_CHECK(!DiagHasErrors(&diag));
    STRATA_CHECK_EQ((long)mod->structs.count, 3);
    STRATA_CHECK(((StructDecl*)VecGet(&mod->structs, 0))->isExtern == false);
    STRATA_CHECK(((StructDecl*)VecGet(&mod->structs, 2))->isExtern == true);
    STRATA_CHECK_EQ((long)mod->enums.count, 2);
    STRATA_CHECK_EQ((long)((EnumDecl*)VecGet(&mod->enums, 0))->members.count, 2);
    STRATA_CHECK_EQ((long)((EnumDecl*)VecGet(&mod->enums, 1))->members.count, 2);

    DiagnosticEngineFree(&diag);
    arena_free(&arena);
}

STRATA_TEST(parser_forward_decl_still_needs_semicolon)
{
    Arena arena; arena_init(&arena, 0);
    DiagnosticEngine diag; DiagnosticEngineInit(&diag);
    ParseModule("struct Foo\nstruct Bar { int x; };", &diag, &arena);
    STRATA_CHECK(DiagHasErrors(&diag));

    DiagnosticEngineFree(&diag);
    arena_free(&arena);
}

STRATA_TEST(parser_extern_return_param)
{
    Arena arena; arena_init(&arena, 0);
    DiagnosticEngine diag; DiagnosticEngineInit(&diag);
    Module* mod = ParseModule(
        "struct Name { int x; };\n"
        "extern void GetName(return Name n);\n"
        "extern void GetValue(const return int v);\n",
        &diag, &arena);
    STRATA_CHECK(!DiagHasErrors(&diag));

    FunctionDecl* getter = (FunctionDecl*)VecGet(&mod->functions, 0);
    STRATA_CHECK(getter->isExtern);
    STRATA_CHECK(getter->hasReturnParam);
    STRATA_CHECK(strcmp(getter->returnType.name, "Name") == 0);
    STRATA_CHECK_EQ((long)getter->params.count, 1);

    ParamDecl* p = (ParamDecl*)VecGet(&getter->params, 0);
    STRATA_CHECK(p->isReturn);
    STRATA_CHECK(p->mod == ModRef);
    STRATA_CHECK(strcmp(p->type.name, "Name") == 0);

    FunctionDecl* constGetter = (FunctionDecl*)VecGet(&mod->functions, 1);
    STRATA_CHECK(constGetter->hasReturnParam);
    STRATA_CHECK(strcmp(constGetter->returnType.name, "int") == 0);
    STRATA_CHECK(!constGetter->returnType.isConst);

    DiagnosticEngineFree(&diag);
    arena_free(&arena);
}

STRATA_TEST(parser_extern_return_param_errors)
{
    struct { const char* src; const char* msg; } cases[] = {
        {"void f(return int x) {}", "'return' parameter is only allowed on extern"},
        {"extern void f(int a, return int b) {}", "must be the last parameter"},
        {"extern int f(return int x);", "must declare 'void' return"},
    };

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
    {
        Arena arena; arena_init(&arena, 0);
        DiagnosticEngine diag; DiagnosticEngineInit(&diag);
        ParseModule(cases[i].src, &diag, &arena);
        STRATA_CHECK(DiagHasErrors(&diag));
        DiagnosticEngineFree(&diag);
        arena_free(&arena);
    }
}

STRATA_TEST(parser_binary_precedence)
{
    Arena arena; arena_init(&arena, 0);
    DiagnosticEngine diag; DiagnosticEngineInit(&diag);
    Module* mod = ParseModule("int f() { return 1 + 2 * 3; }", &diag, &arena);
    STRATA_CHECK(!DiagHasErrors(&diag));

    ReturnStmt* ret = SingleReturn(mod);
    STRATA_CHECK(ret->value->kind == NodeBinary);

    BinaryExpr* add = (BinaryExpr*)ret->value;
    STRATA_CHECK(add->op == BinAdd);
    STRATA_CHECK(add->lhs->kind == NodeIntLiteral);
    STRATA_CHECK(add->rhs->kind == NodeBinary);

    BinaryExpr* mul = (BinaryExpr*)add->rhs;
    STRATA_CHECK(mul->op == BinMul);

    DiagnosticEngineFree(&diag);
    arena_free(&arena);
}

STRATA_TEST(parser_var_decl)
{
    Arena arena; arena_init(&arena, 0);
    DiagnosticEngine diag; DiagnosticEngineInit(&diag);
    Module* mod = ParseModule("int f() { int x = 5; return x; }", &diag, &arena);
    STRATA_CHECK(!DiagHasErrors(&diag));

    FunctionDecl* fn = (FunctionDecl*)VecGet(&mod->functions, 0);
    Block* block = (Block*)fn->body;
    STRATA_CHECK(((Node*)VecGet(&block->statements, 0))->kind == NodeVarDecl);

    VarDeclStmt* vd = (VarDeclStmt*)VecGet(&block->statements, 0);
    STRATA_CHECK(strcmp(vd->name, "x") == 0);
    STRATA_CHECK(vd->init != NULL);
    STRATA_CHECK(vd->init->kind == NodeIntLiteral);

    DiagnosticEngineFree(&diag);
    arena_free(&arena);
}

STRATA_TEST(parser_call_and_member)
{
    Arena arena; arena_init(&arena, 0);
    DiagnosticEngine diag; DiagnosticEngineInit(&diag);
    Module* mod = ParseModule("int f() { return add(1, 2); }", &diag, &arena);
    STRATA_CHECK(!DiagHasErrors(&diag));

    ReturnStmt* ret = SingleReturn(mod);
    STRATA_CHECK(ret->value->kind == NodeCall);

    CallExpr* call = (CallExpr*)ret->value;
    STRATA_CHECK(strcmp(call->callee, "add") == 0);
    STRATA_CHECK_EQ((long)call->args.count, 2);

    DiagnosticEngineFree(&diag);
    arena_free(&arena);
}

STRATA_TEST(parser_recovers_from_error)
{
    Arena arena; arena_init(&arena, 0);
    DiagnosticEngine diag; DiagnosticEngineInit(&diag);
    Module* mod = ParseModule("int f( { }", &diag, &arena);
    STRATA_CHECK(DiagHasErrors(&diag));
    STRATA_CHECK(DiagErrorCount(&diag) >= 1);
    DiagnosticEngineFree(&diag);
    arena_free(&arena);
}

STRATA_TEST(parser_if_else)
{
    Arena arena; arena_init(&arena, 0);
    DiagnosticEngine diag; DiagnosticEngineInit(&diag);
    Module* mod = ParseModule("int f(int a) { if (a) { return 1; } else { return 2; } }", &diag, &arena);
    STRATA_CHECK(!DiagHasErrors(&diag));

    FunctionDecl* fn = (FunctionDecl*)VecGet(&mod->functions, 0);
    Block* block = (Block*)fn->body;
    STRATA_CHECK(((Node*)VecGet(&block->statements, 0))->kind == NodeIf);

    IfStmt* ifn = (IfStmt*)VecGet(&block->statements, 0);
    STRATA_CHECK(ifn->thenBranch != NULL);
    STRATA_CHECK(ifn->elseBranch != NULL);

    DiagnosticEngineFree(&diag);
    arena_free(&arena);
}

static size_t CountDiagsContaining(const DiagnosticEngine* diag, const char* needle)
{
    size_t n = 0;

    for (size_t i = 0; i < diag->m_count; i++)
    {
        if (strstr(diag->m_diagnostics[i].message, needle))
        {
            n++;
        }
    }

    return n;
}

static Node* StatementAt(Module* m, size_t fnIndex, size_t stmtIndex)
{
    if (!m || fnIndex >= m->functions.count)
    {
        return NULL;
    }

    FunctionDecl* fn = (FunctionDecl*)VecGet(&m->functions, fnIndex);
    Block* block = (Block*)fn->body;

    if (!block || stmtIndex >= block->statements.count)
    {
        return NULL;
    }

    return (Node*)VecGet(&block->statements, stmtIndex);
}

STRATA_TEST(parser_long_string_literal_is_not_truncated)
{
    size_t n = 70000;
    Sb sb;
    SbInit(&sb);
    SbPuts(&sb, "int f() { string s = \"");
    SbPutr(&sb, 'a', n);
    SbPuts(&sb, "\"; return 0; }");

    Arena arena; arena_init(&arena, 0);
    DiagnosticEngine diag; DiagnosticEngineInit(&diag);
    char* src = SbFinish(&sb, &arena);
    Module* mod = ParseModule(src, &diag, &arena);
    STRATA_CHECK(!DiagHasErrors(&diag));

    VarDeclStmt* vd = (VarDeclStmt*)StatementAt(mod, 0, 0);
    STRATA_CHECK(vd && vd->init && vd->init->kind == NodeStrLiteral);
    if (vd && vd->init && vd->init->kind == NodeStrLiteral)
    {
        StrLiteral* lit = (StrLiteral*)vd->init;
        STRATA_CHECK_EQ((long)lit->length, (long)n);
        STRATA_CHECK_EQ((long)strlen(lit->value), (long)n);
        STRATA_CHECK_EQ((long)lit->base.range.length, (long)(n + 2));
    }

    DiagnosticEngineFree(&diag);
    arena_free(&arena);
}

STRATA_TEST(parser_string_literal_keeps_embedded_nul)
{
    Arena arena; arena_init(&arena, 0);
    DiagnosticEngine diag; DiagnosticEngineInit(&diag);
    Module* mod = ParseModule("int f() { string s = \"ab\\0cd\"; string e = \"\"; return 0; }", &diag, &arena);
    STRATA_CHECK(!DiagHasErrors(&diag));

    VarDeclStmt* vd = (VarDeclStmt*)StatementAt(mod, 0, 0);
    StrLiteral* lit = (StrLiteral*)vd->init;
    STRATA_CHECK(lit->base.kind == NodeStrLiteral);
    STRATA_CHECK_EQ((long)lit->length, 5);
    STRATA_CHECK(memcmp(lit->value, "ab\0cd", 5) == 0);

    VarDeclStmt* empty = (VarDeclStmt*)StatementAt(mod, 0, 1);
    STRATA_CHECK_EQ((long)((StrLiteral*)empty->init)->length, 0);

    DiagnosticEngineFree(&diag);
    arena_free(&arena);
}

STRATA_TEST(parser_paren_ident_before_binary_op_is_an_expression)
{
    /* `(x) - 1` used to be parsed as a cast of `-1` to type `x`. */
    const char* ops[] = {"-", "+", "*", "&", "/", "=="};

    for (size_t i = 0; i < sizeof(ops) / sizeof(ops[0]); i++)
    {
        char src[128];
        snprintf(src, sizeof(src), "int f(int x) { return (x) %s 1; }", ops[i]);

        Arena arena; arena_init(&arena, 0);
        DiagnosticEngine diag; DiagnosticEngineInit(&diag);
        Module* mod = ParseModule(src, &diag, &arena);
        STRATA_CHECK(!DiagHasErrors(&diag));

        ReturnStmt* ret = SingleReturn(mod);
        STRATA_CHECK(ret && ret->value && ret->value->kind == NodeBinary);
        if (ret && ret->value && ret->value->kind == NodeBinary)
        {
            STRATA_CHECK(((BinaryExpr*)ret->value)->lhs->kind == NodeIdent);
        }

        DiagnosticEngineFree(&diag);
        arena_free(&arena);
    }

    /* `(x)++` is a postfix increment of x, not a cast of `++...`. */
    Arena arena; arena_init(&arena, 0);
    DiagnosticEngine diag; DiagnosticEngineInit(&diag);
    Module* mod = ParseModule("void f(int x) { (x)++; (x)--; }", &diag, &arena);
    STRATA_CHECK(!DiagHasErrors(&diag));

    ExprStmt* es = (ExprStmt*)StatementAt(mod, 0, 0);
    STRATA_CHECK(es && es->expr && es->expr->kind == NodeIncDec);
    if (es && es->expr && es->expr->kind == NodeIncDec)
    {
        STRATA_CHECK(!((IncDecExpr*)es->expr)->isPrefix);
    }

    DiagnosticEngineFree(&diag);
    arena_free(&arena);
}

STRATA_TEST(parser_unambiguous_casts_still_parse_as_casts)
{
    const char* exprs[] = {"(Foo)y", "(Foo)(-y)", "(Foo)!y", "(Foo)~y", "(Foo)1", "(Foo)\"s\"",
                           "(int)-y", "(long)+y", "(float)y", "(^Foo)y", "(Foo[])y", "(string)y"};

    for (size_t i = 0; i < sizeof(exprs) / sizeof(exprs[0]); i++)
    {
        char src[128];
        snprintf(src, sizeof(src), "int f(int y) { return %s; }", exprs[i]);

        Arena arena; arena_init(&arena, 0);
        DiagnosticEngine diag; DiagnosticEngineInit(&diag);
        Module* mod = ParseModule(src, &diag, &arena);
        STRATA_CHECK(!DiagHasErrors(&diag));

        ReturnStmt* ret = SingleReturn(mod);
        STRATA_CHECK(ret && ret->value && ret->value->kind == NodeCast);
        if (!ret || !ret->value || ret->value->kind != NodeCast)
        {
            printf("  not a cast: %s\n", exprs[i]);
        }

        DiagnosticEngineFree(&diag);
        arena_free(&arena);
    }
}

STRATA_TEST(parser_int_literals_are_decimal_everywhere)
{
    /* Array sizes and fieldoffset used strtoull base 0: `010` was 8 there
       (but 10 in expressions) and `09` was 0. */
    Arena arena; arena_init(&arena, 0);
    DiagnosticEngine diag; DiagnosticEngineInit(&diag);
    Module* mod = ParseModule("extern struct E { fieldoffset(010) int a; }\n"
                              "int f() { int[010] a = {}; int[09] b = {}; int[0x10] c = {}; return 010; }",
                              &diag, &arena);
    STRATA_CHECK(!DiagHasErrors(&diag));

    StructDecl* sd = (StructDecl*)VecGet(&mod->structs, 0);
    FieldDecl* field = (FieldDecl*)VecGet(&sd->fields, 0);
    STRATA_CHECK_EQ(field->offset, 10);

    STRATA_CHECK_EQ(((VarDeclStmt*)StatementAt(mod, 0, 0))->type.length, 10);
    STRATA_CHECK_EQ(((VarDeclStmt*)StatementAt(mod, 0, 1))->type.length, 9);
    STRATA_CHECK_EQ(((VarDeclStmt*)StatementAt(mod, 0, 2))->type.length, 16);
    STRATA_CHECK(strcmp(((VarDeclStmt*)StatementAt(mod, 0, 0))->type.name, "int[10]") == 0);

    ReturnStmt* ret = (ReturnStmt*)StatementAt(mod, 0, 3);
    STRATA_CHECK_EQ((long)((IntLiteral*)ret->value)->value, 10);

    DiagnosticEngineFree(&diag);
    arena_free(&arena);
}

STRATA_TEST(parser_int_literal_range_and_long_spellings)
{
    Arena arena; arena_init(&arena, 0);
    DiagnosticEngine diag; DiagnosticEngineInit(&diag);

    /* 70 leading zeros used to be cut to 63 chars (all zeros) by a fixed buffer. */
    Sb sb;
    SbInit(&sb);
    SbPuts(&sb, "ulong f() { return ");
    SbPutr(&sb, '0', 70);
    SbPuts(&sb, "1; }\nulong g() { return 18446744073709551615u; }\nulong h() { return 0xFFFFFFFFFFFFFFFF; }");
    Module* mod = ParseModule(SbFinish(&sb, &arena), &diag, &arena);
    STRATA_CHECK(!DiagHasErrors(&diag));

    STRATA_CHECK_EQ((long)((IntLiteral*)((ReturnStmt*)StatementAt(mod, 0, 0))->value)->value, 1);
    STRATA_CHECK(((IntLiteral*)((ReturnStmt*)StatementAt(mod, 1, 0))->value)->value == UINT64_MAX);
    STRATA_CHECK(((IntLiteral*)((ReturnStmt*)StatementAt(mod, 2, 0))->value)->value == UINT64_MAX);

    DiagnosticEngineFree(&diag);
    arena_free(&arena);

    const char* bad[] = {"ulong f() { return 18446744073709551616; }", "ulong f() { return 0x10000000000000000; }",
                         "int f() { int[99999999999999999999] a = {}; return 0; }"};

    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++)
    {
        arena_init(&arena, 0);
        DiagnosticEngineInit(&diag);
        ParseModule(bad[i], &diag, &arena);
        STRATA_CHECK_EQ(CountDiagsContaining(&diag, "too large to fit in 64 bits"), 1);
        DiagnosticEngineFree(&diag);
        arena_free(&arena);
    }
}

STRATA_TEST(parser_float_literal_out_of_range_is_diagnosed)
{
    Arena arena; arena_init(&arena, 0);
    DiagnosticEngine diag; DiagnosticEngineInit(&diag);
    ParseModule("double f() { return 1e999; }", &diag, &arena);
    STRATA_CHECK_EQ(CountDiagsContaining(&diag, "out of range"), 1);
    DiagnosticEngineFree(&diag);
    arena_free(&arena);
}

STRATA_TEST(parser_box_cannot_be_optional)
{
    /* The inner type parse used to swallow the `?`, so `^S?` was accepted silently. */
    Arena arena; arena_init(&arena, 0);
    DiagnosticEngine diag; DiagnosticEngineInit(&diag);
    Module* mod = ParseModule("struct S { int v; }\nstruct T { ^S? f; }", &diag, &arena);
    STRATA_CHECK_EQ(CountDiagsContaining(&diag, "'^S' cannot be optional"), 1);
    STRATA_CHECK_EQ(DiagErrorCount(&diag), 1);

    /* Recovery keeps the field as a plain box. */
    StructDecl* t = (StructDecl*)VecGet(&mod->structs, 1);
    STRATA_CHECK_EQ((long)t->fields.count, 1);
    FieldDecl* f = (FieldDecl*)VecGet(&t->fields, 0);
    STRATA_CHECK(f->type.isBox && !f->type.isOptional);

    DiagnosticEngineFree(&diag);
    arena_free(&arena);
}

STRATA_TEST(parser_box_array_optional_shapes)
{
    Arena arena; arena_init(&arena, 0);
    DiagnosticEngine diag; DiagnosticEngineInit(&diag);
    Module* mod = ParseModule("int f() { ^S[]? a; ^S[] b = {}; ^S[4] c = {}; return 0; }", &diag, &arena);
    STRATA_CHECK(!DiagHasErrors(&diag));

    /* `^S[]?` is an optional array of boxes, not a box around an optional array. */
    TypeName* a = &((VarDeclStmt*)StatementAt(mod, 0, 0))->type;
    STRATA_CHECK(a->isOptional && !a->isBox);
    STRATA_CHECK(a->inner && a->inner->isArray && a->inner->length < 0);
    STRATA_CHECK(a->inner && a->inner->elem && a->inner->elem->isBox);
    STRATA_CHECK(strcmp(a->name, "^S[]?") == 0);

    TypeName* b = &((VarDeclStmt*)StatementAt(mod, 0, 1))->type;
    STRATA_CHECK(b->isArray && b->length < 0 && b->elem->isBox);
    STRATA_CHECK(strcmp(b->name, "^S[]") == 0);

    TypeName* c = &((VarDeclStmt*)StatementAt(mod, 0, 2))->type;
    STRATA_CHECK(c->isArray && c->length == 4 && c->elem->isBox);
    STRATA_CHECK(strcmp(c->name, "^S[4]") == 0);

    DiagnosticEngineFree(&diag);
    arena_free(&arena);

    arena_init(&arena, 0);
    DiagnosticEngineInit(&diag);
    ParseModule("int f() { ^S[4]? a; return 0; }", &diag, &arena);
    STRATA_CHECK_EQ(CountDiagsContaining(&diag, "fixed-size array '^S[4]' cannot be optional"), 1);
    DiagnosticEngineFree(&diag);
    arena_free(&arena);
}

STRATA_TEST(parser_trial_parse_diagnostics_are_rolled_back)
{
    /* The cast trial parse read `arr[0]?` as a type and left an error behind. */
    Arena arena; arena_init(&arena, 0);
    DiagnosticEngine diag; DiagnosticEngineInit(&diag);
    Module* mod = ParseModule("struct S { int v; }\nint f() { S?[] arr = {}; bool b = (arr[0]?); return 0; }", &diag,
                              &arena);
    STRATA_CHECK(!DiagHasErrors(&diag));
    STRATA_CHECK_EQ((long)DiagCount(&diag), 0);

    VarDeclStmt* vd = (VarDeclStmt*)StatementAt(mod, 0, 1);
    STRATA_CHECK(vd->init && vd->init->kind == NodeNullTest);

    DiagnosticEngineFree(&diag);
    arena_free(&arena);
}

STRATA_TEST(parser_lexer_error_is_reported_once_after_backtracking)
{
    Arena arena; arena_init(&arena, 0);
    DiagnosticEngine diag; DiagnosticEngineInit(&diag);
    ParseModule("int f() { foo $; return 0; }", &diag, &arena);
    STRATA_CHECK_EQ(CountDiagsContaining(&diag, "unexpected character '$'"), 1);
    DiagnosticEngineFree(&diag);
    arena_free(&arena);
}

STRATA_TEST(parser_dispose_frees_array_literal_and_enum_value_lists)
{
    Arena arena; arena_init(&arena, 0);
    DiagnosticEngine diag; DiagnosticEngineInit(&diag);
    Module* mod = ParseModule("enum E { A = g(1, 2), B }\nint f() { int[] a = {1, 2, 3}; return 0; }", &diag, &arena);
    STRATA_CHECK(!DiagHasErrors(&diag));

    ArrayInitExpr* ai = (ArrayInitExpr*)((VarDeclStmt*)StatementAt(mod, 0, 0))->init;
    STRATA_CHECK(ai->base.kind == NodeArrayInit);
    STRATA_CHECK_EQ((long)ai->elements.count, 3);

    EnumDecl* ed = (EnumDecl*)VecGet(&mod->enums, 0);
    EnumMemberDecl* a = (EnumMemberDecl*)VecGet(&ed->members, 0);
    CallExpr* call = (CallExpr*)a->valueExpr;
    STRATA_CHECK(call->base.kind == NodeCall);
    STRATA_CHECK_EQ((long)call->args.count, 2);

    /* Nodes live in the arena, so they stay readable after AstDispose frees their lists. */
    AstDispose((Node*)mod);
    STRATA_CHECK(ai->elements.items == NULL && ai->elements.count == 0);
    STRATA_CHECK(call->args.items == NULL && call->args.count == 0);

    DiagnosticEngineFree(&diag);
    arena_free(&arena);
}

STRATA_TEST(parser_error_paths_dispose_partial_nodes)
{
    /* Exercise the error paths that now dispose partially built nodes. */
    const char* srcs[] = {"int f() { return g(1, 2) + ; }", "int f(int[] a) { return a[h(1, 2)][; }",
                          "handle H;\nimpl H { extern int M(int a); extern int M(int b); }"};

    for (size_t i = 0; i < sizeof(srcs) / sizeof(srcs[0]); i++)
    {
        Arena arena; arena_init(&arena, 0);
        DiagnosticEngine diag; DiagnosticEngineInit(&diag);
        Module* mod = ParseModule(srcs[i], &diag, &arena);
        STRATA_CHECK(DiagHasErrors(&diag));
        AstDispose((Node*)mod);
        DiagnosticEngineFree(&diag);
        arena_free(&arena);
    }
}

STRATA_TEST(util_sb_cdup_on_empty_builder)
{
    Sb sb;
    SbInit(&sb);
    Str s = SbCDup(&sb);
    STRATA_CHECK_EQ((long)s.len, 1);
    STRATA_CHECK(s.data[0] == '\0');
    free((char*)s.data);
    SbFree(&sb);
}

static char* FormatTwice(Arena* a, char** second, const char* fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    char* first = arena_vformat(a, fmt, args);
    *second = arena_vformat(a, fmt, args);
    va_end(args);
    return first;
}

STRATA_TEST(util_arena_vformat_leaves_callers_va_list_alone)
{
    Arena arena; arena_init(&arena, 0);
    char* second = NULL;
    char* first = FormatTwice(&arena, &second, "%s-%d", "x", 42);
    STRATA_CHECK(strcmp(first, "x-42") == 0);
    STRATA_CHECK(strcmp(second, "x-42") == 0);
    arena_free(&arena);
}
