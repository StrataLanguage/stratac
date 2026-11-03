// Builder: Numeric operators: binary ops, checked division and shifts, casts.
#include "Codegen/Builder/BuilderInternal.h"

// `(int)f`: the float is clamped to the target's range and NaN becomes 0 (LLVM's saturating conversion), instead of
// the undefined result of a plain fptosi/fptoui on an out-of-range value. Vectors and odd targets convert plainly.
LLVMValueRef BuildFloatToIntSaturating(Builder* b, LLVMValueRef value, LLVMTypeRef target, bool isUnsigned)
{
    LLVMTypeRef src = LLVMTypeOf(value);
    LLVMTypeKind srcKind = LLVMGetTypeKind(src);

    if (LLVMGetTypeKind(target) != LLVMIntegerTypeKind
        || (srcKind != LLVMFloatTypeKind && srcKind != LLVMDoubleTypeKind))
    {
        return isUnsigned ? LLVMBuildFPToUI(b->builder, value, target, "c")
                          : LLVMBuildFPToSI(b->builder, value, target, "c");
    }

    char name[64];
    snprintf(name, sizeof(name), "llvm.fpto%ci.sat.i%u.%s", isUnsigned ? 'u' : 's', LLVMGetIntTypeWidth(target),
             srcKind == LLVMFloatTypeKind ? "f32" : "f64");

    LLVMTypeRef params[1] = {src};
    LLVMTypeRef fnTy = LLVMFunctionType(target, params, 1, 0);
    LLVMValueRef fn = LLVMGetNamedFunction(b->mod, name);

    if (!fn)
    {
        fn = LLVMAddFunction(b->mod, name, fnTy);
    }

    LLVMValueRef args[1] = {value};
    return LLVMBuildCall2(b->builder, fnTy, fn, args, 1, "sat");
}

// A vector comparison yields one bool per lane; `==` holds when every lane does. Scalars pass through.
static LLVMValueRef AllLanesTrue(Builder* b, LLVMValueRef cmp)
{
    if (LLVMGetTypeKind(LLVMTypeOf(cmp)) != LLVMVectorTypeKind)
    {
        return cmp;
    }

    unsigned lanes = LLVMGetVectorSize(LLVMTypeOf(cmp));
    LLVMValueRef all = LLVMBuildExtractElement(b->builder, cmp, LLVMConstInt(I32Ty(b), 0, 0), "lane");

    for (unsigned i = 1; i < lanes; i++)
    {
        LLVMValueRef lane = LLVMBuildExtractElement(b->builder, cmp, LLVMConstInt(I32Ty(b), i, 0), "lane");
        all = LLVMBuildAnd(b->builder, all, lane, "lanes");
    }

    return all;
}

// Integer `/` and `%` trap in hardware on a zero divisor and on MIN / -1; both are panics instead.
static void EmitDivisionChecks(Builder* b, TypeDesc td, LLVMValueRef lhs, LLVMValueRef divisor, const char* zeroMessage,
                               const char* overflowMessage)
{
    LLVMTypeRef ty = LLVMTypeOf(divisor);

    if (LLVMGetTypeKind(ty) != LLVMIntegerTypeKind)
    {
        return;
    }

    EmitPanicIf(b, LLVMBuildICmp(b->builder, LLVMIntEQ, divisor, LLVMConstInt(ty, 0, 0), "div.zero"), zeroMessage);

    if (!TypeDescIsUnsigned(&td))
    {
        unsigned width = LLVMGetIntTypeWidth(ty);
        LLVMValueRef minVal = LLVMConstInt(ty, 1ULL << (width - 1), 0);
        LLVMValueRef isMin = LLVMBuildICmp(b->builder, LLVMIntEQ, lhs, minVal, "div.min");
        LLVMValueRef isNeg1 = LLVMBuildICmp(b->builder, LLVMIntEQ, divisor, LLVMConstInt(ty, ~0ULL, 1), "div.neg1");
        EmitPanicIf(b, LLVMBuildAnd(b->builder, isMin, isNeg1, "div.ovf"), overflowMessage);
    }
}

// A shift by the operand's width or more is undefined in LLVM and masked by the hardware: a panic instead.
static void EmitShiftCheck(Builder* b, LLVMValueRef value, LLVMValueRef count)
{
    LLVMTypeRef ty = LLVMTypeOf(count);

    if (LLVMGetTypeKind(ty) != LLVMIntegerTypeKind || LLVMGetTypeKind(LLVMTypeOf(value)) != LLVMIntegerTypeKind)
    {
        return;
    }

    unsigned width = LLVMGetIntTypeWidth(LLVMTypeOf(value));
    LLVMValueRef limit = LLVMConstInt(ty, width, 0);
    // Unsigned compare: a negative count is a huge one.
    EmitPanicIf(b, LLVMBuildICmp(b->builder, LLVMIntUGE, count, limit, "shift.big"), "shift count out of range");
}

static bool IsComparisonOp(BinaryOp op)
{
    return op == BinEqEq || op == BinNotEq || op == BinLt || op == BinLtEq || op == BinGt || op == BinGtEq;
}

// `(Color)n` at run time: an integer that is not the value of some member is not a Color, so it panics instead of
// becoming a value no `switch`/comparison anticipates. Constants were checked by sema.
void EmitEnumCastCheck(Builder* b, CastExpr* cast, Value operand, TypeDesc target)
{
    const EnumDecl* ed = ModuleFindEnum(b->module, cast->type.name);

    if (!ed || ed->members.count == 0 || !operand.value || TypeDescIsFloat(&operand.typeDesc)
        || TypeDescIsFloat(&target) || !target.type || LLVMGetTypeKind(target.type) != LLVMIntegerTypeKind
        || LLVMGetTypeKind(LLVMTypeOf(operand.value)) != LLVMIntegerTypeKind || cast->operand->kind == NodeIntLiteral
        || (cast->operand->kind == NodeMember && ((MemberExpr*)cast->operand)->isEnumConst))
    {
        return;
    }

    Value converted = Coerce(b, operand, target);
    LLVMValueRef isMember = LLVMConstInt(I1Ty(b), 0, 0);

    for (size_t i = 0; i < ed->members.count; i++)
    {
        const EnumMemberDecl* member = (const EnumMemberDecl*)VecGet((Vec*)&ed->members, i);
        uint64_t raw = member->isNegative ? (uint64_t)(-(int64_t)member->value) : member->value;
        LLVMValueRef want = LLVMConstInt(target.type, raw, 0);
        isMember = LLVMBuildOr(b->builder, isMember,
                               LLVMBuildICmp(b->builder, LLVMIntEQ, converted.value, want, "enum.is"), "enum.any");
    }

    EmitPanicIf(b, LLVMBuildNot(b->builder, isMember, "enum.bad"), "enum cast: value is not a member");
}

// `l op r` for integers of different signedness. Both are widened to 64 bits (sign- or zero-extended as their
// own type says); a negative signed side is below every unsigned value, otherwise the unsigned compare decides.
static Value EmitMixedSignCompare(Builder* b, BinaryOp op, Value l, Value r)
{
    bool leftSigned = !TypeDescIsUnsigned(&l.typeDesc);
    Value sv = leftSigned ? l : r;
    Value uv = leftSigned ? r : l;
    LLVMValueRef s64 = LLVMBuildSExt(b->builder, sv.value, I64Ty(b), "mix.s");
    LLVMValueRef u64 = LLVMBuildZExt(b->builder, uv.value, I64Ty(b), "mix.u");

    if (LLVMGetIntTypeWidth(sv.typeDesc.type) == 64)
    {
        s64 = sv.value;
    }

    if (LLVMGetIntTypeWidth(uv.typeDesc.type) == 64)
    {
        u64 = uv.value;
    }

    LLVMValueRef neg = LLVMBuildICmp(b->builder, LLVMIntSLT, s64, LLVMConstInt(I64Ty(b), 0, 0), "mix.neg");

    // Put the comparison in terms of (signed, unsigned): `u < s` is `s > u`.
    BinaryOp sop = op;

    if (!leftSigned)
    {
        sop = op == BinLt ? BinGt : op == BinLtEq ? BinGtEq : op == BinGt ? BinLt : op == BinGtEq ? BinLtEq : op;
    }

    LLVMValueRef out = NULL;
    TypeDesc boolDesc = TypeDescMake(I1Ty(b), 0, NULL);

    switch (sop)
    {
    case BinLt:
        out = LLVMBuildOr(b->builder, neg, LLVMBuildICmp(b->builder, LLVMIntULT, s64, u64, "mix.c"), "mix.lt");
        break;
    case BinLtEq:
        out = LLVMBuildOr(b->builder, neg, LLVMBuildICmp(b->builder, LLVMIntULE, s64, u64, "mix.c"), "mix.le");
        break;
    case BinGt:
        out = LLVMBuildAnd(b->builder, LLVMBuildNot(b->builder, neg, "mix.pos"),
                           LLVMBuildICmp(b->builder, LLVMIntUGT, s64, u64, "mix.c"), "mix.gt");
        break;
    case BinGtEq:
        out = LLVMBuildAnd(b->builder, LLVMBuildNot(b->builder, neg, "mix.pos"),
                           LLVMBuildICmp(b->builder, LLVMIntUGE, s64, u64, "mix.c"), "mix.ge");
        break;
    case BinEqEq:
        out = LLVMBuildAnd(b->builder, LLVMBuildNot(b->builder, neg, "mix.pos"),
                           LLVMBuildICmp(b->builder, LLVMIntEQ, s64, u64, "mix.c"), "mix.eq");
        break;
    default:
        out = LLVMBuildOr(b->builder, neg, LLVMBuildICmp(b->builder, LLVMIntNE, s64, u64, "mix.c"), "mix.ne");
        break;
    }

    return ValueMake(out, boolDesc);
}

// `a && b` and `a || b`: the right side is evaluated only when the left does not decide the result.
static Value EmitLogicalBinary(Builder* b, BinaryExpr* n)
{
    bool isAnd = (n->op == BinLogicAnd);

    LLVMValueRef cond = ToI1(b, EmitCondOperand(b, n->lhs));

    LLVMBasicBlockRef lhsEnd = LLVMGetInsertBlock(b->builder);
    LLVMBasicBlockRef rhsBlock = LLVMAppendBasicBlockInContext(b->ctx, b->curFn, "logic.rhs");
    LLVMBasicBlockRef mergeBlock = LLVMAppendBasicBlockInContext(b->ctx, b->curFn, "logic.end");

    if (isAnd)
    {
        LLVMBuildCondBr(b->builder, cond, rhsBlock, mergeBlock);
    }
    else
    {
        LLVMBuildCondBr(b->builder, cond, mergeBlock, rhsBlock);
    }

    LLVMPositionBuilderAtEnd(b->builder, rhsBlock);
    LLVMValueRef rhsCond = ToI1(b, EmitCondOperand(b, n->rhs));

    LLVMBasicBlockRef rhsEnd = LLVMGetInsertBlock(b->builder);
    LLVMBuildBr(b->builder, mergeBlock);

    LLVMPositionBuilderAtEnd(b->builder, mergeBlock);
    LLVMValueRef phi = LLVMBuildPhi(b->builder, I1Ty(b), "logic");

    LLVMValueRef shortCircuit = isAnd ? LLVMConstNull(I1Ty(b)) : LLVMConstInt(I1Ty(b), 1, 0);

    LLVMBasicBlockRef incomingBlocks[2] = {lhsEnd, rhsEnd};
    LLVMValueRef incomingVals[2] = {shortCircuit, rhsCond};
    LLVMAddIncoming(phi, incomingVals, incomingBlocks, 2);

    return ValueMake(phi, TypeDescMake(I1Ty(b), 0, NULL));
}

// `==`/`!=` on two box or optional operands (^T, T?) compare the pointed-to values
// structurally (member-wise for structs, content for strings, element-wise for
// arrays). `T?` is null-aware: empty == empty, empty != non-empty. Incomplete-struct
// inners hold the T* itself, so their deref is identity; handle inners hold the
// handle value, so they load. Deep equality of cyclic T? graphs does not terminate.
static Value EmitBoxEquality(Builder* b, BinaryExpr* n, Value l, Value r)
{
    LLVMBasicBlockRef falseBB = NewBb(b, "boxeq.f");
    LLVMBasicBlockRef trueBB = NewBb(b, "boxeq.t");
    LLVMBasicBlockRef eqEnd;

    if (l.typeDesc.boxInner)
    {
        TypeDesc innerTd = Resolve(b, l.typeDesc.boxInner);
        eqEnd = EmitBoxCellEq(b, b->curFn, l.value, r.value, &innerTd, TypeDescIsOptional(&l.typeDesc), falseBB);
        LLVMBuildBr(b->builder, trueBB);
    }
    else
    {
        // ^string (degenerate: the cell IS a char*): identity.
        LLVMBuildCondBr(b->builder, LLVMBuildICmp(b->builder, LLVMIntEQ, l.value, r.value, "boxeq"), trueBB, falseBB);
        eqEnd = LLVMGetInsertBlock(b->builder);
    }

    LLVMPositionBuilderAtEnd(b->builder, falseBB);
    LLVMBuildBr(b->builder, trueBB);
    LLVMPositionBuilderAtEnd(b->builder, trueBB);
    LLVMValueRef cmp = LLVMBuildPhi(b->builder, I1Ty(b), "boxeq");
    LLVMValueRef inVals[2] = {LLVMConstInt(I1Ty(b), 1, 0), LLVMConstInt(I1Ty(b), 0, 0)};
    LLVMBasicBlockRef inBlocks[2] = {eqEnd, falseBB};
    LLVMAddIncoming(cmp, inVals, inBlocks, 2);

    if (n->op == BinNotEq)
    {
        cmp = LLVMBuildNot(b->builder, cmp, "boxne");
    }

    return ValueMake(cmp, TypeDescMake(I1Ty(b), 0, NULL));
}

// String / cstring equality is CONTENT comparison (strata_str_eq with a
// length fast-out and pointer-identity fast-in) — never a raw compare of
// the pointers. `cstring` supplies its length via strlen.
static Value EmitStringEquality(Builder* b, BinaryExpr* n, Value l, Value r)
{
    // Both sides are only read: `make_str() == "x"` temporaries.
    BorrowTemp(b, l, TypeDescMake(I1Ty(b), 0, NULL));
    BorrowTemp(b, r, TypeDescMake(I1Ty(b), 0, NULL));

    LLVMValueRef lp;
    LLVMValueRef ll;
    LLVMValueRef rp;
    LLVMValueRef rl;

    if (TypeDescIsCString(&l.typeDesc))
    {
        lp = l.value;
        ll = BuildStrLen(b, l.value);
    }
    else
    {
        lp = LLVMBuildExtractValue(b->builder, l.value, 0, "seq.l");
        ll = WidenLen(b, LLVMBuildExtractValue(b->builder, l.value, 1, "seq.ll"));
    }

    if (TypeDescIsCString(&r.typeDesc))
    {
        rp = r.value;
        rl = BuildStrLen(b, r.value);
    }
    else
    {
        rp = LLVMBuildExtractValue(b->builder, r.value, 0, "seq.r");
        rl = WidenLen(b, LLVMBuildExtractValue(b->builder, r.value, 1, "seq.rl"));
    }

    LLVMValueRef args[4] = {lp, ll, rp, rl};
    StrataStrEqFn(b);
    LLVMValueRef wide = LLVMBuildCall2(b->builder, b->strEqFnType, b->strEqFn, args, 4, "seq");
    LLVMValueRef cmp = LLVMBuildICmp(b->builder, LLVMIntNE, wide, LLVMConstInt(I32Ty(b), 0, 0), "seq.b");

    if (n->op == BinNotEq)
    {
        cmp = LLVMBuildNot(b->builder, cmp, "sne");
    }

    return ValueMake(cmp, TypeDescMake(I1Ty(b), 0, NULL));
}

// Concatenation: both sides are read (temporaries die at statement end); the result is
// a fresh owned string.
static Value EmitStringConcat(Builder* b, Value l, Value r)
{
    ClaimTemp(b, l);
    ClaimTemp(b, r);

    LLVMValueRef lp = TypeDescIsCString(&l.typeDesc) ? l.value : LLVMBuildExtractValue(b->builder, l.value, 0, "cat.l");
    LLVMValueRef ll = TypeDescIsCString(&l.typeDesc)
                          ? BuildStrLen(b, l.value)
                          : WidenLen(b, LLVMBuildExtractValue(b->builder, l.value, 1, "cat.ll"));
    LLVMValueRef rp = TypeDescIsCString(&r.typeDesc) ? r.value : LLVMBuildExtractValue(b->builder, r.value, 0, "cat.r");
    LLVMValueRef rl = TypeDescIsCString(&r.typeDesc)
                          ? BuildStrLen(b, r.value)
                          : WidenLen(b, LLVMBuildExtractValue(b->builder, r.value, 1, "cat.rl"));

    Value result = ValueMake(BuildConcatStringFat(b, lp, ll, rp, rl), StringFatDesc(b));
    NoteFreshOwned(b, result);

    return result;
}

// Strings support only `==`, `!=` and `+`; any other operator is a diagnosed error.
static Value EmitStringBinary(Builder* b, BinaryExpr* n, Value l, Value r)
{
    if (n->op == BinEqEq || n->op == BinNotEq)
    {
        return EmitStringEquality(b, n, l, r);
    }

    if (n->op == BinAdd)
    {
        return EmitStringConcat(b, l, r);
    }

    DiagError(b->diag, n->base.range, "strings only support '==' and '!=' comparisons");

    return ValueMake(LLVMConstNull(I1Ty(b)), TypeDescMake(I1Ty(b), 0, NULL));
}

// Structs and arrays (other than strings and opaque handles) are aggregate
// operands of `==`/`!=`.
static bool IsAggregateEqOperand(Builder* b, TypeDesc* td)
{
    return (td->structTypeName != NULL && !TypeRegistryIsOpaque(&b->registry, td->structTypeName))
           || ((TypeDescHasArrayShape(td) || TypeDescIsFixedArray(td)) && !TypeDescIsString(td));
}

// Aggregate equality: structs and arrays compare structurally — a
// whole-value byte compare where the layout allows (zero-initialized
// padding, no floats), member-wise / element-wise recursion otherwise
// (IEEE-754 floats, strcmp-style strings, nested arrays). The operands
// were loaded as values above: spill each to a temp so the per-type
// helper can compare through addresses.
static Value EmitAggregateEquality(Builder* b, BinaryExpr* n, Value l, Value r)
{
    LLVMValueRef la = SpillEqOperand(b, l);
    LLVMValueRef ra = SpillEqOperand(b, r);
    LLVMTypeRef eqParams[2] = {b->ptrTy, b->ptrTy};
    LLVMTypeRef eqFnTy = LLVMFunctionType(I1Ty(b), eqParams, 2, 0);
    LLVMValueRef eqArgs[2] = {la, ra};
    LLVMValueRef eq = LLVMBuildCall2(b->builder, eqFnTy, EnsureEqHelper(b, &l.typeDesc, false), eqArgs, 2, "eq");

    if (n->op == BinNotEq)
    {
        eq = LLVMBuildNot(b->builder, eq, "eqne");
    }

    return ValueMake(eq, TypeDescMake(I1Ty(b), 0, NULL));
}

// A signed value against an unsigned one compares by value, not by whichever signedness the operands happen
// to be coerced to: -1 < 1u is true, and a long is never equal to an ulong it merely shares bits with.
static bool IsMixedSignIntCompare(BinaryOp op, Value l, Value r)
{
    return IsComparisonOp(op) && !TypeDescIsFloat(&l.typeDesc) && !TypeDescIsFloat(&r.typeDesc) && l.typeDesc.type
           && r.typeDesc.type && LLVMGetTypeKind(l.typeDesc.type) == LLVMIntegerTypeKind
           && LLVMGetTypeKind(r.typeDesc.type) == LLVMIntegerTypeKind && LLVMGetIntTypeWidth(l.typeDesc.type) > 1
           && LLVMGetIntTypeWidth(r.typeDesc.type) > 1
           && TypeDescIsUnsigned(&l.typeDesc) != TypeDescIsUnsigned(&r.typeDesc);
}

// Brings two numeric operands to one type: a float meets a float, a mixed float / double pair computes
// in double, and mixed integer widths compute in the wider type. Returns the operation's type.
static TypeDesc NormalizeNumericOperands(Builder* b, Value* l, Value* r)
{
    TypeDesc typeDesc = l->typeDesc;

    if (TypeDescIsFloat(&l->typeDesc) && !TypeDescIsFloat(&r->typeDesc))
    {
        *r = Coerce(b, *r, l->typeDesc);
    }
    else if (!TypeDescIsFloat(&l->typeDesc) && TypeDescIsFloat(&r->typeDesc))
    {
        *l = Coerce(b, *l, r->typeDesc);

        typeDesc = r->typeDesc;
    }
    else if (TypeDescIsFloat(&l->typeDesc) && TypeDescIsFloat(&r->typeDesc) && l->typeDesc.type != r->typeDesc.type)
    {
        // float mixed with double: compute in the wider type (fpext the float
        // side); mismatched widths would emit invalid IR like `fmul float X, double Y`.
        LLVMTypeRef d = LLVMDoubleTypeInContext(b->ctx);

        if (l->typeDesc.type == d)
        {
            *r = Coerce(b, *r, l->typeDesc);
            typeDesc = l->typeDesc;
        }
        else
        {
            *l = Coerce(b, *l, r->typeDesc);
            typeDesc = r->typeDesc;
        }
    }
    else if (!TypeDescIsFloat(&l->typeDesc) && !TypeDescIsFloat(&r->typeDesc) && l->typeDesc.type != r->typeDesc.type
             && l->typeDesc.type && r->typeDesc.type)
    {
        // Mixed integer widths: compute in the wider type (`int + byte`
        // must not truncate the int to i8). Non-integer operands (pointers
        // etc.) keep the left-to-right coercion.
        bool bothInt = LLVMGetTypeKind(l->typeDesc.type) == LLVMIntegerTypeKind
                       && LLVMGetTypeKind(r->typeDesc.type) == LLVMIntegerTypeKind;
        bool leftWider = bothInt ? LLVMGetIntTypeWidth(l->typeDesc.type) > LLVMGetIntTypeWidth(r->typeDesc.type)
                                 : l->typeDesc.type == I64Ty(b);

        if (leftWider)
        {
            *r = Coerce(b, *r, l->typeDesc);
        }
        else
        {
            *l = Coerce(b, *l, r->typeDesc);
            typeDesc = r->typeDesc;
        }
    }

    return typeDesc;
}

// For comparisons involving pointer types (string, box, handle), convert
// both operands to i64 so that icmp works correctly.
static void PromotePointerComparison(Builder* b, BinaryOp op, Value* l, Value* r, TypeDesc* typeDesc, bool* flt)
{
    if (!(typeDesc->type == b->ptrTy || r->typeDesc.type == b->ptrTy))
    {
        return;
    }

    switch (op)
    {
    case BinEqEq:
    case BinNotEq:
    case BinLt:
    case BinLtEq:
    case BinGt:
    case BinGtEq:
        *typeDesc = TypeDescMake(I64Ty(b), TDF_UNSIGNED, NULL);
        *flt = false;
        if (l->typeDesc.type == b->ptrTy)
        {
            l->value = LLVMBuildPtrToInt(b->builder, l->value, I64Ty(b), "picmp");
            l->typeDesc = *typeDesc;
        }
        else
        {
            *l = Coerce(b, *l, *typeDesc);
        }
        if (r->typeDesc.type == b->ptrTy)
        {
            r->value = LLVMBuildPtrToInt(b->builder, r->value, I64Ty(b), "picmp");
            r->typeDesc = *typeDesc;
        }
        else
        {
            *r = Coerce(b, *r, *typeDesc);
        }
        break;
    default:
        break;
    }
}

// Integer and float `+ - * / %`. Integer `/` and `%` are checked for a zero divisor and
// for overflow before the operation.
static Value EmitNumericBinary(Builder* b, BinaryOp op, Value l, Value r, TypeDesc typeDesc, bool flt)
{
    LLVMValueRef out = NULL;

    switch (op)
    {
    case BinAdd:
        out = flt ? LLVMBuildFAdd(b->builder, l.value, r.value, "add")
                  : LLVMBuildAdd(b->builder, l.value, r.value, "add");

        return ValueMake(out, typeDesc);

    case BinSub:
        out = flt ? LLVMBuildFSub(b->builder, l.value, r.value, "sub")
                  : LLVMBuildSub(b->builder, l.value, r.value, "sub");

        return ValueMake(out, typeDesc);

    case BinMul:
        out = flt ? LLVMBuildFMul(b->builder, l.value, r.value, "mul")
                  : LLVMBuildMul(b->builder, l.value, r.value, "mul");

        return ValueMake(out, typeDesc);

    case BinDiv:
        if (!flt)
        {
            EmitDivisionChecks(b, typeDesc, l.value, r.value, "integer division by zero", "integer division overflows");
        }

        out = flt ? LLVMBuildFDiv(b->builder, l.value, r.value, "div")
                  : (TypeDescIsUnsigned(&typeDesc) ? LLVMBuildUDiv(b->builder, l.value, r.value, "div")
                                                   : LLVMBuildSDiv(b->builder, l.value, r.value, "div"));

        return ValueMake(out, typeDesc);

    case BinMod:
        if (!flt)
        {
            EmitDivisionChecks(b, typeDesc, l.value, r.value, "integer modulo by zero", "integer modulo overflows");
        }

        out = flt ? LLVMBuildFRem(b->builder, l.value, r.value, "mod")
                  : (TypeDescIsUnsigned(&typeDesc) ? LLVMBuildURem(b->builder, l.value, r.value, "mod")
                                                   : LLVMBuildSRem(b->builder, l.value, r.value, "mod"));

        return ValueMake(out, typeDesc);

    default:
        // Not an arithmetic operator: the dispatcher never sends one here.
        return l;
    }
}

// Integer-only bitwise operators and shifts. A shift by the operand's width or more
// is undefined in LLVM and masked by the hardware, so it panics instead.
static Value EmitBitwiseBinary(Builder* b, BinaryOp op, Value l, Value r, TypeDesc typeDesc)
{
    LLVMValueRef out = NULL;

    switch (op)
    {
    case BinBitAnd:
        out = LLVMBuildAnd(b->builder, l.value, r.value, "and");

        return ValueMake(out, typeDesc);

    case BinBitOr:
        out = LLVMBuildOr(b->builder, l.value, r.value, "or");

        return ValueMake(out, typeDesc);

    case BinBitXor:
        out = LLVMBuildXor(b->builder, l.value, r.value, "xor");

        return ValueMake(out, typeDesc);

    case BinShl:
        EmitShiftCheck(b, l.value, r.value);
        out = LLVMBuildShl(b->builder, l.value, r.value, "shl");

        return ValueMake(out, typeDesc);

    case BinShr:
        EmitShiftCheck(b, l.value, r.value);
        out = TypeDescIsUnsigned(&typeDesc) ? LLVMBuildLShr(b->builder, l.value, r.value, "shr")
                                            : LLVMBuildAShr(b->builder, l.value, r.value, "shr");

        return ValueMake(out, typeDesc);

    default:
        // Not a bitwise operator: the dispatcher never sends one here.
        return l;
    }
}

// The six comparisons on integers, floats and vectors. A vector comparison yields one
// bool per lane; `==` holds when every lane does.
static Value EmitComparisonBinary(Builder* b, BinaryOp op, Value l, Value r, TypeDesc typeDesc, bool flt)
{
    LLVMValueRef out = NULL;
    TypeDesc boolTypeDesc = TypeDescMake(I1Ty(b), 0, NULL);

    switch (op)
    {
    case BinEqEq:
        out = flt ? FcmpByName(b->builder, "oeq", l.value, r.value)
                  : IcmpByName(b->builder, "eq", TypeDescIsUnsigned(&typeDesc), l.value, r.value);

        return ValueMake(AllLanesTrue(b, out), boolTypeDesc);

    case BinNotEq:
        // Vectors differ when they are not all-equal; scalars keep the ordered `one`.
        if (flt && LLVMGetTypeKind(LLVMTypeOf(l.value)) == LLVMVectorTypeKind)
        {
            out = LLVMBuildNot(b->builder, AllLanesTrue(b, FcmpByName(b->builder, "oeq", l.value, r.value)), "vne");

            return ValueMake(out, boolTypeDesc);
        }

        out = flt ? FcmpByName(b->builder, "one", l.value, r.value)
                  : IcmpByName(b->builder, "ne", TypeDescIsUnsigned(&typeDesc), l.value, r.value);

        return ValueMake(AllLanesTrue(b, out), boolTypeDesc);

    case BinLt:
        out = flt ? FcmpByName(b->builder, "olt", l.value, r.value)
                  : IcmpByName(b->builder, "lt", TypeDescIsUnsigned(&typeDesc), l.value, r.value);

        return ValueMake(out, boolTypeDesc);

    case BinLtEq:
        out = flt ? FcmpByName(b->builder, "ole", l.value, r.value)
                  : IcmpByName(b->builder, "le", TypeDescIsUnsigned(&typeDesc), l.value, r.value);

        return ValueMake(out, boolTypeDesc);

    case BinGt:
        out = flt ? FcmpByName(b->builder, "ogt", l.value, r.value)
                  : IcmpByName(b->builder, "gt", TypeDescIsUnsigned(&typeDesc), l.value, r.value);

        return ValueMake(out, boolTypeDesc);

    case BinGtEq:
        out = flt ? FcmpByName(b->builder, "oge", l.value, r.value)
                  : IcmpByName(b->builder, "ge", TypeDescIsUnsigned(&typeDesc), l.value, r.value);

        return ValueMake(out, boolTypeDesc);

    default:
        // Not a comparison: the dispatcher never sends one here.
        return l;
    }
}

Value EmitBinary(Builder* b, BinaryExpr* n)
{
    if (n->op == BinLogicAnd || n->op == BinLogicOr)
    {
        return EmitLogicalBinary(b, n);
    }

    Value l = EmitExpr(b, n->lhs);
    Value r = EmitExpr(b, n->rhs);

    if (TypeDescIsBox(&l.typeDesc) && TypeDescIsBox(&r.typeDesc) && (n->op == BinEqEq || n->op == BinNotEq))
    {
        return EmitBoxEquality(b, n, l, r);
    }

    l = DerefBoxValue(b, l);
    r = DerefBoxValue(b, r);

    if (TypeDescIsString(&l.typeDesc) || TypeDescIsString(&r.typeDesc) || TypeDescIsCString(&l.typeDesc)
        || TypeDescIsCString(&r.typeDesc))
    {
        return EmitStringBinary(b, n, l, r);
    }

    if ((n->op == BinEqEq || n->op == BinNotEq) && IsAggregateEqOperand(b, &l.typeDesc)
        && IsAggregateEqOperand(b, &r.typeDesc))
    {
        return EmitAggregateEquality(b, n, l, r);
    }

    if (IsMixedSignIntCompare(n->op, l, r))
    {
        return EmitMixedSignCompare(b, n->op, l, r);
    }

    TypeDesc typeDesc = NormalizeNumericOperands(b, &l, &r);
    bool flt = TypeDescIsFloat(&typeDesc);

    PromotePointerComparison(b, n->op, &l, &r, &typeDesc, &flt);

    switch (n->op)
    {
    case BinAdd:
    case BinSub:
    case BinMul:
    case BinDiv:
    case BinMod:
        return EmitNumericBinary(b, n->op, l, r, typeDesc, flt);

    case BinBitAnd:
    case BinBitOr:
    case BinBitXor:
    case BinShl:
    case BinShr:
        return EmitBitwiseBinary(b, n->op, l, r, typeDesc);

    case BinEqEq:
    case BinNotEq:
    case BinLt:
    case BinLtEq:
    case BinGt:
    case BinGtEq:
        return EmitComparisonBinary(b, n->op, l, r, typeDesc, flt);

    default:
        return l;
    }
}

Value EmitIntLiteral(Builder* b, IntLiteral* literal)
{
    // A magnitude that can't be represented as a non-negative signed
    // i32 (>= 2^31) must be a 64-bit literal: LLVMConstInt on an i32 would sign-wrap it and lose the value
    // (`long a = 3000000000` would otherwise become negative).
    if (literal->value > 0x7FFFFFFFULL || literal->isLong)
    {
        TypeDesc typeDesc = TypeDescMake(I64Ty(b), (literal->isUnsigned ? TDF_UNSIGNED : 0), NULL);

        return ValueMake(LLVMConstInt(I64Ty(b), literal->value, 1), typeDesc);
    }

    TypeDesc typeDesc = TypeDescMake(I32Ty(b), (literal->isUnsigned ? TDF_UNSIGNED : 0), NULL);

    return ValueMake(LLVMConstInt(I32Ty(b), literal->value, 1), typeDesc);
}

Value EmitFloatLiteral(Builder* b, FloatLiteral* literal)
{
    LLVMTypeRef fty = LLVMFloatTypeInContext(b->ctx);
    TypeDesc typeDesc = TypeDescMake(fty, TDF_FLOAT, NULL);

    return ValueMake(LLVMConstReal(fty, literal->value), typeDesc);
}

Value EmitBoolLiteral(Builder* b, BoolLiteral* literal)
{
    TypeDesc typeDesc = TypeDescMake(I1Ty(b), 0, NULL);

    return ValueMake(LLVMConstInt(I1Ty(b), (unsigned long long)literal->value, 0), typeDesc);
}

Value EmitIncDec(Builder* b, IncDecExpr* inc)
{
    LValue lv = EmitLValue(b, inc->operand);

    if (!lv.valid)
    {
        if (b->diag)
        {
            DiagError(b->diag, inc->base.range, "increment/decrement of non-lvalue");
        }

        return ValueMake(NULL, TypeDescMake(NULL, TDF_VOID, NULL));
    }

    if (lv.vectorLane)
    {
        LLVMValueRef cur = LLVMBuildLoad2(b->builder, lv.vectorType, lv.ptr, "lanev");
        LLVMValueRef laneVal = LLVMBuildExtractElement(b->builder, cur, LLVMConstInt(I32Ty(b), lv.lane, 0), "lanecur");
        LLVMValueRef one = LLVMConstReal(LLVMFloatTypeInContext(b->ctx), 1.0);
        LLVMValueRef newVal = inc->isDec ? LLVMBuildFSub(b->builder, laneVal, one, "dec")
                                         : LLVMBuildFAdd(b->builder, laneVal, one, "inc");
        LLVMValueRef newVec
            = LLVMBuildInsertElement(b->builder, cur, newVal, LLVMConstInt(I32Ty(b), lv.lane, 0), "laneins");
        LLVMBuildStore(b->builder, newVec, lv.ptr);

        return ValueMake(inc->isPrefix ? newVal : laneVal, lv.typeDesc);
    }

    LLVMValueRef valPtr = lv.ptr;
    TypeDesc valTd = lv.typeDesc;

    if (TypeDescIsBox(&lv.typeDesc))
    {
        // lv.ptr is the address of the box pointer slot (ref-adjusted
        // already by EmitLValue); load through it to get the box
        // pointer, then read/write the boxed value through that -
        // same pattern as the box branch in EmitAssign.
        valPtr = LLVMBuildLoad2(b->builder, b->ptrTy, lv.ptr, "box");
        valTd = Resolve(b, lv.typeDesc.boxInner);
    }

    LLVMValueRef cur = LLVMBuildLoad2(b->builder, valTd.type, valPtr, "inc");
    LLVMValueRef one = TypeDescIsFloat(&valTd) ? LLVMConstReal(valTd.type, 1.0) : LLVMConstInt(valTd.type, 1, 0);

    LLVMValueRef newVal = inc->isDec ? (TypeDescIsFloat(&valTd) ? LLVMBuildFSub(b->builder, cur, one, "dec")
                                                                : LLVMBuildSub(b->builder, cur, one, "dec"))
                                     : (TypeDescIsFloat(&valTd) ? LLVMBuildFAdd(b->builder, cur, one, "inc")
                                                                : LLVMBuildAdd(b->builder, cur, one, "inc"));

    LLVMBuildStore(b->builder, newVal, valPtr);

    return ValueMake(inc->isPrefix ? newVal : cur, valTd);
}

Value EmitCast(Builder* b, CastExpr* cast)
{
    if (cast->textKind != CastTextNone)
    {
        return EmitTextCast(b, cast);
    }

    Value operand = EmitExpr(b, cast->operand);
    TypeDesc target = Resolve(b, &cast->type);

    EmitEnumCastCheck(b, cast, operand, target);

    // `(Name)"lit"` / `(string)"lit"`: the result must OWN its bytes, so
    // heap-copy the static literal (drop glue frees real memory, never
    // the constant pool). Movable sources stay a retag - the consumer
    // nulls the source, exactly like `string t = s;`.
    if (TypeDescIsString(&target) && cast->operand->kind == NodeStrLiteral)
    {
        LLVMValueRef srcPtr = LLVMBuildExtractValue(b->builder, operand.value, 0, "clit.p");
        LLVMValueRef lenVal = LLVMBuildExtractValue(b->builder, operand.value, 1, "clit.l");
        LLVMValueRef owned = BuildOwnedStringFat(b, srcPtr, lenVal);

        return ValueMake(owned, target);
    }

    if ((TypeDescIsBox(&operand.typeDesc) && TypeDescIsBox(&target))
        || (TypeDescIsString(&operand.typeDesc) && TypeDescIsString(&target)))
    {
        // Retag with the destination type; Coerce would keep the source's.
        return ValueMake(operand.value, target);
    }

    // Pointer to pointer (handle <-> handle, ptr <-> cstring): same bits, the destination's type.
    bool operandIsPointer = operand.typeDesc.type && LLVMGetTypeKind(operand.typeDesc.type) == LLVMPointerTypeKind
                            && !TypeDescIsBox(&operand.typeDesc);
    bool targetIsPointer
        = target.type && LLVMGetTypeKind(target.type) == LLVMPointerTypeKind && !TypeDescIsBox(&target);

    if (operandIsPointer && targetIsPointer)
    {
        return ValueMake(operand.value, target);
    }

    return Coerce(b, operand, target);
}
