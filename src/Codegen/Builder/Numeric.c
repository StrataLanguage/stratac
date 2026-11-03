/* Builder: Numeric operators: binary ops, checked division and shifts, casts. */
#include "Codegen/Builder/BuilderInternal.h"

/* `(int)f`: the float is clamped to the target's range and NaN becomes 0 (LLVM's saturating conversion), instead of
   the undefined result of a plain fptosi/fptoui on an out-of-range value. Vectors and odd targets convert plainly. */
LLVMValueRef BuildFloatToIntSaturating(Builder* b, LLVMValueRef value, LLVMTypeRef target, bool isUnsigned)
{
    LLVMTypeRef src = LLVMTypeOf(value);
    LLVMTypeKind srcKind = LLVMGetTypeKind(src);

    if (LLVMGetTypeKind(target) != LLVMIntegerTypeKind
        || (srcKind != LLVMFloatTypeKind && srcKind != LLVMDoubleTypeKind))
    {
        return isUnsigned ? LLVMBuildFPToUI(b->m_builder, value, target, "c")
                          : LLVMBuildFPToSI(b->m_builder, value, target, "c");
    }

    char name[64];
    snprintf(name, sizeof(name), "llvm.fpto%ci.sat.i%u.%s", isUnsigned ? 'u' : 's', LLVMGetIntTypeWidth(target),
             srcKind == LLVMFloatTypeKind ? "f32" : "f64");

    LLVMTypeRef params[1] = {src};
    LLVMTypeRef fnTy = LLVMFunctionType(target, params, 1, 0);
    LLVMValueRef fn = LLVMGetNamedFunction(b->m_mod, name);

    if (!fn)
    {
        fn = LLVMAddFunction(b->m_mod, name, fnTy);
    }

    LLVMValueRef args[1] = {value};
    return LLVMBuildCall2(b->m_builder, fnTy, fn, args, 1, "sat");
}

/* A vector comparison yields one bool per lane; `==` holds when every lane does. Scalars pass through. */
static LLVMValueRef AllLanesTrue(Builder* b, LLVMValueRef cmp)
{
    if (LLVMGetTypeKind(LLVMTypeOf(cmp)) != LLVMVectorTypeKind)
    {
        return cmp;
    }

    unsigned lanes = LLVMGetVectorSize(LLVMTypeOf(cmp));
    LLVMValueRef all = LLVMBuildExtractElement(b->m_builder, cmp, LLVMConstInt(I32Ty(b), 0, 0), "lane");

    for (unsigned i = 1; i < lanes; i++)
    {
        LLVMValueRef lane = LLVMBuildExtractElement(b->m_builder, cmp, LLVMConstInt(I32Ty(b), i, 0), "lane");
        all = LLVMBuildAnd(b->m_builder, all, lane, "lanes");
    }

    return all;
}

/* Integer `/` and `%` trap in hardware on a zero divisor and on MIN / -1; both are panics instead. */
static void EmitDivisionChecks(Builder* b, TypeDesc td, LLVMValueRef lhs, LLVMValueRef divisor, const char* zeroMessage,
                               const char* overflowMessage)
{
    LLVMTypeRef ty = LLVMTypeOf(divisor);

    if (LLVMGetTypeKind(ty) != LLVMIntegerTypeKind)
    {
        return;
    }

    EmitPanicIf(b, LLVMBuildICmp(b->m_builder, LLVMIntEQ, divisor, LLVMConstInt(ty, 0, 0), "div.zero"), zeroMessage);

    if (!td.isUnsigned)
    {
        unsigned width = LLVMGetIntTypeWidth(ty);
        LLVMValueRef minVal = LLVMConstInt(ty, 1ULL << (width - 1), 0);
        LLVMValueRef isMin = LLVMBuildICmp(b->m_builder, LLVMIntEQ, lhs, minVal, "div.min");
        LLVMValueRef isNeg1 = LLVMBuildICmp(b->m_builder, LLVMIntEQ, divisor, LLVMConstInt(ty, ~0ULL, 1), "div.neg1");
        EmitPanicIf(b, LLVMBuildAnd(b->m_builder, isMin, isNeg1, "div.ovf"), overflowMessage);
    }
}

/* A shift by the operand's width or more is undefined in LLVM and masked by the hardware: a panic instead. */
static void EmitShiftCheck(Builder* b, LLVMValueRef value, LLVMValueRef count)
{
    LLVMTypeRef ty = LLVMTypeOf(count);

    if (LLVMGetTypeKind(ty) != LLVMIntegerTypeKind || LLVMGetTypeKind(LLVMTypeOf(value)) != LLVMIntegerTypeKind)
    {
        return;
    }

    unsigned width = LLVMGetIntTypeWidth(LLVMTypeOf(value));
    LLVMValueRef limit = LLVMConstInt(ty, width, 0);
    /* Unsigned compare: a negative count is a huge one. */
    EmitPanicIf(b, LLVMBuildICmp(b->m_builder, LLVMIntUGE, count, limit, "shift.big"), "shift count out of range");
}

static bool IsComparisonOp(BinaryOp op)
{
    return op == BinEqEq || op == BinNotEq || op == BinLt || op == BinLtEq || op == BinGt || op == BinGtEq;
}

/* `l op r` for integers of different signedness. Both are widened to 64 bits (sign- or zero-extended as their
   own type says); a negative signed side is below every unsigned value, otherwise the unsigned compare decides. */
/* `(Color)n` at run time: an integer that is not the value of some member is not a Color, so it panics instead of
   becoming a value no `switch`/comparison anticipates. Constants were checked by sema. */
void EmitEnumCastCheck(Builder* b, CastExpr* cast, Value operand, TypeDesc target)
{
    const EnumDecl* ed = ModuleFindEnum(b->m_module, cast->type.name);

    if (!ed || ed->members.count == 0 || !operand.value || operand.typeDesc.isFloat || target.isFloat || !target.type
        || LLVMGetTypeKind(target.type) != LLVMIntegerTypeKind
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
        isMember = LLVMBuildOr(b->m_builder, isMember,
                               LLVMBuildICmp(b->m_builder, LLVMIntEQ, converted.value, want, "enum.is"), "enum.any");
    }

    EmitPanicIf(b, LLVMBuildNot(b->m_builder, isMember, "enum.bad"), "enum cast: value is not a member");
}

static Value EmitMixedSignCompare(Builder* b, BinaryOp op, Value l, Value r)
{
    bool leftSigned = !l.typeDesc.isUnsigned;
    Value sv = leftSigned ? l : r;
    Value uv = leftSigned ? r : l;
    LLVMValueRef s64 = LLVMBuildSExt(b->m_builder, sv.value, I64Ty(b), "mix.s");
    LLVMValueRef u64 = LLVMBuildZExt(b->m_builder, uv.value, I64Ty(b), "mix.u");

    if (LLVMGetIntTypeWidth(sv.typeDesc.type) == 64)
    {
        s64 = sv.value;
    }

    if (LLVMGetIntTypeWidth(uv.typeDesc.type) == 64)
    {
        u64 = uv.value;
    }

    LLVMValueRef neg = LLVMBuildICmp(b->m_builder, LLVMIntSLT, s64, LLVMConstInt(I64Ty(b), 0, 0), "mix.neg");

    /* Put the comparison in terms of (signed, unsigned): `u < s` is `s > u`. */
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
        out = LLVMBuildOr(b->m_builder, neg, LLVMBuildICmp(b->m_builder, LLVMIntULT, s64, u64, "mix.c"), "mix.lt");
        break;
    case BinLtEq:
        out = LLVMBuildOr(b->m_builder, neg, LLVMBuildICmp(b->m_builder, LLVMIntULE, s64, u64, "mix.c"), "mix.le");
        break;
    case BinGt:
        out = LLVMBuildAnd(b->m_builder, LLVMBuildNot(b->m_builder, neg, "mix.pos"),
                           LLVMBuildICmp(b->m_builder, LLVMIntUGT, s64, u64, "mix.c"), "mix.gt");
        break;
    case BinGtEq:
        out = LLVMBuildAnd(b->m_builder, LLVMBuildNot(b->m_builder, neg, "mix.pos"),
                           LLVMBuildICmp(b->m_builder, LLVMIntUGE, s64, u64, "mix.c"), "mix.ge");
        break;
    case BinEqEq:
        out = LLVMBuildAnd(b->m_builder, LLVMBuildNot(b->m_builder, neg, "mix.pos"),
                           LLVMBuildICmp(b->m_builder, LLVMIntEQ, s64, u64, "mix.c"), "mix.eq");
        break;
    default:
        out = LLVMBuildOr(b->m_builder, neg, LLVMBuildICmp(b->m_builder, LLVMIntNE, s64, u64, "mix.c"), "mix.ne");
        break;
    }

    return ValueMake(out, boolDesc);
}

Value EmitBinary(Builder* b, BinaryExpr* n)
{
    if (n->op == BinLogicAnd || n->op == BinLogicOr)
    {
        bool isAnd = (n->op == BinLogicAnd);

        LLVMValueRef cond = ToI1(b, EmitCondOperand(b, n->lhs));

        LLVMBasicBlockRef lhsEnd = LLVMGetInsertBlock(b->m_builder);
        LLVMBasicBlockRef rhsBlock = LLVMAppendBasicBlockInContext(b->m_ctx, b->m_curFn, "logic.rhs");
        LLVMBasicBlockRef mergeBlock = LLVMAppendBasicBlockInContext(b->m_ctx, b->m_curFn, "logic.end");

        if (isAnd)
        {
            LLVMBuildCondBr(b->m_builder, cond, rhsBlock, mergeBlock);
        }
        else
        {
            LLVMBuildCondBr(b->m_builder, cond, mergeBlock, rhsBlock);
        }

        LLVMPositionBuilderAtEnd(b->m_builder, rhsBlock);
        LLVMValueRef rhsCond = ToI1(b, EmitCondOperand(b, n->rhs));

        LLVMBasicBlockRef rhsEnd = LLVMGetInsertBlock(b->m_builder);
        LLVMBuildBr(b->m_builder, mergeBlock);

        LLVMPositionBuilderAtEnd(b->m_builder, mergeBlock);
        LLVMValueRef phi = LLVMBuildPhi(b->m_builder, I1Ty(b), "logic");

        LLVMValueRef shortCircuit = isAnd ? LLVMConstNull(I1Ty(b)) : LLVMConstInt(I1Ty(b), 1, 0);

        LLVMBasicBlockRef incomingBlocks[2] = {lhsEnd, rhsEnd};
        LLVMValueRef incomingVals[2] = {shortCircuit, rhsCond};
        LLVMAddIncoming(phi, incomingVals, incomingBlocks, 2);

        return ValueMake(phi, TypeDescMake(I1Ty(b), 0, NULL));
    }

    Value l = EmitExpr(b, n->lhs);
    Value r = EmitExpr(b, n->rhs);

    /* Box/optional operands (^T, T?) AUTO-DEREF like everywhere else:
        `==`/`!=` compare the pointed-to values structurally (member-wise for
        structs, content for strings, element-wise for arrays). `T?` is
        null-aware: empty == empty, empty != non-empty. Incomplete-struct
        inners hold the T* itself, so their deref is identity; handle inners
        hold the handle value, so they load. (An explicit identity intrinsic
        may come later; deep equality of cyclic T? graphs does not
        terminate.) */
    if ((TypeDescIsBox(&l.typeDesc) && TypeDescIsBox(&r.typeDesc)) && (n->op == BinEqEq || n->op == BinNotEq))
    {
        LLVMBasicBlockRef falseBB = NewBb(b, "boxeq.f");
        LLVMBasicBlockRef trueBB = NewBb(b, "boxeq.t");
        LLVMBasicBlockRef eqEnd;

        if (l.typeDesc.boxInner)
        {
            TypeDesc innerTd = Resolve(b, l.typeDesc.boxInner);
            eqEnd = EmitBoxCellEq(b, b->m_curFn, l.value, r.value, &innerTd, TypeDescIsOptional(&l.typeDesc), falseBB);
            LLVMBuildBr(b->m_builder, trueBB);
        }
        else
        {
            /* ^string (degenerate: the cell IS a char*): identity. */
            LLVMBuildCondBr(b->m_builder, LLVMBuildICmp(b->m_builder, LLVMIntEQ, l.value, r.value, "boxeq"), trueBB,
                            falseBB);
            eqEnd = LLVMGetInsertBlock(b->m_builder);
        }

        LLVMPositionBuilderAtEnd(b->m_builder, falseBB);
        LLVMBuildBr(b->m_builder, trueBB);
        LLVMPositionBuilderAtEnd(b->m_builder, trueBB);
        LLVMValueRef cmp = LLVMBuildPhi(b->m_builder, I1Ty(b), "boxeq");
        LLVMValueRef inVals[2] = {LLVMConstInt(I1Ty(b), 1, 0), LLVMConstInt(I1Ty(b), 0, 0)};
        LLVMBasicBlockRef inBlocks[2] = {eqEnd, falseBB};
        LLVMAddIncoming(cmp, inVals, inBlocks, 2);

        if (n->op == BinNotEq)
        {
            cmp = LLVMBuildNot(b->m_builder, cmp, "boxne");
        }

        return ValueMake(cmp, TypeDescMake(I1Ty(b), 0, NULL));
    }

    l = DerefBoxValue(b, l);
    r = DerefBoxValue(b, r);

    TypeDesc typeDesc = l.typeDesc;

    /* String / cstring equality is CONTENT comparison (strata_str_eq with a
       length fast-out and pointer-identity fast-in) — never a raw compare of
       the pointers. `cstring` supplies its length via strlen. Other
       comparisons are meaningless. */
    if (l.typeDesc.isString || r.typeDesc.isString || l.typeDesc.isCString || r.typeDesc.isCString)
    {
        if (n->op == BinEqEq || n->op == BinNotEq)
        {
            /* Both sides are only read: `make_str() == "x"` temporaries. */
            BorrowTemp(b, l, TypeDescMake(I1Ty(b), 0, NULL));
            BorrowTemp(b, r, TypeDescMake(I1Ty(b), 0, NULL));

            LLVMValueRef lp;
            LLVMValueRef ll;
            LLVMValueRef rp;
            LLVMValueRef rl;

            if (l.typeDesc.isCString)
            {
                lp = l.value;
                ll = BuildStrLen(b, l.value);
            }
            else
            {
                lp = LLVMBuildExtractValue(b->m_builder, l.value, 0, "seq.l");
                ll = WidenLen(b, LLVMBuildExtractValue(b->m_builder, l.value, 1, "seq.ll"));
            }

            if (r.typeDesc.isCString)
            {
                rp = r.value;
                rl = BuildStrLen(b, r.value);
            }
            else
            {
                rp = LLVMBuildExtractValue(b->m_builder, r.value, 0, "seq.r");
                rl = WidenLen(b, LLVMBuildExtractValue(b->m_builder, r.value, 1, "seq.rl"));
            }

            LLVMValueRef args[4] = {lp, ll, rp, rl};
            StrataStrEqFn(b);
            LLVMValueRef wide = LLVMBuildCall2(b->m_builder, b->m_strEqFnType, b->m_strEqFn, args, 4, "seq");
            LLVMValueRef cmp = LLVMBuildICmp(b->m_builder, LLVMIntNE, wide, LLVMConstInt(I32Ty(b), 0, 0), "seq.b");

            if (n->op == BinNotEq)
            {
                cmp = LLVMBuildNot(b->m_builder, cmp, "sne");
            }

            return ValueMake(cmp, TypeDescMake(I1Ty(b), 0, NULL));
        }

        if (n->op == BinAdd)
        {
            /* Concatenation: both sides are read (temporaries die at statement end); the result is
               a fresh owned string. */
            ClaimTemp(b, l);
            ClaimTemp(b, r);

            LLVMValueRef lp = l.typeDesc.isCString ? l.value : LLVMBuildExtractValue(b->m_builder, l.value, 0, "cat.l");
            LLVMValueRef ll = l.typeDesc.isCString
                                  ? BuildStrLen(b, l.value)
                                  : WidenLen(b, LLVMBuildExtractValue(b->m_builder, l.value, 1, "cat.ll"));
            LLVMValueRef rp = r.typeDesc.isCString ? r.value : LLVMBuildExtractValue(b->m_builder, r.value, 0, "cat.r");
            LLVMValueRef rl = r.typeDesc.isCString
                                  ? BuildStrLen(b, r.value)
                                  : WidenLen(b, LLVMBuildExtractValue(b->m_builder, r.value, 1, "cat.rl"));

            Value result = ValueMake(BuildConcatStringFat(b, lp, ll, rp, rl), StringFatDesc(b));
            NoteFreshOwned(b, result);
            return result;
        }

        DiagError(b->m_diag, n->base.range, "strings only support '==' and '!=' comparisons");

        return ValueMake(LLVMConstNull(I1Ty(b)), TypeDescMake(I1Ty(b), 0, NULL));
    }

    /* Aggregate equality: structs and arrays compare structurally — a
       whole-value byte compare where the layout allows (zero-initialized
       padding, no floats), member-wise / element-wise recursion otherwise
       (IEEE-754 floats, strcmp-style strings, nested arrays). The operands
       were loaded as values above: spill each to a temp so the per-type
       helper can compare through addresses. */
    if (n->op == BinEqEq || n->op == BinNotEq)
    {
        /* Handles (opaque) are pointers: they compare by address below, never member-wise. */
        bool lAgg
            = (l.typeDesc.structTypeName != NULL && !TypeRegistryIsOpaque(&b->m_registry, l.typeDesc.structTypeName))
              || ((TypeDescHasArrayShape(&l.typeDesc) || l.typeDesc.isFixedArray) && !l.typeDesc.isString);
        bool rAgg
            = (r.typeDesc.structTypeName != NULL && !TypeRegistryIsOpaque(&b->m_registry, r.typeDesc.structTypeName))
              || ((TypeDescHasArrayShape(&r.typeDesc) || r.typeDesc.isFixedArray) && !r.typeDesc.isString);

        if (lAgg && rAgg)
        {
            LLVMValueRef la = SpillEqOperand(b, l);
            LLVMValueRef ra = SpillEqOperand(b, r);
            LLVMTypeRef eqParams[2] = {b->m_ptrTy, b->m_ptrTy};
            LLVMTypeRef eqFnTy = LLVMFunctionType(I1Ty(b), eqParams, 2, 0);
            LLVMValueRef eqArgs[2] = {la, ra};
            LLVMValueRef eq
                = LLVMBuildCall2(b->m_builder, eqFnTy, EnsureEqHelper(b, &l.typeDesc, false), eqArgs, 2, "eq");

            if (n->op == BinNotEq)
            {
                eq = LLVMBuildNot(b->m_builder, eq, "eqne");
            }

            return ValueMake(eq, TypeDescMake(I1Ty(b), 0, NULL));
        }
    }

    /* A signed value against an unsigned one compares by value, not by whichever signedness the operands happen
       to be coerced to: -1 < 1u is true, and a long is never equal to an ulong it merely shares bits with. */
    if (IsComparisonOp(n->op) && !l.typeDesc.isFloat && !r.typeDesc.isFloat && l.typeDesc.type && r.typeDesc.type
        && LLVMGetTypeKind(l.typeDesc.type) == LLVMIntegerTypeKind
        && LLVMGetTypeKind(r.typeDesc.type) == LLVMIntegerTypeKind && LLVMGetIntTypeWidth(l.typeDesc.type) > 1
        && LLVMGetIntTypeWidth(r.typeDesc.type) > 1 && l.typeDesc.isUnsigned != r.typeDesc.isUnsigned)
    {
        return EmitMixedSignCompare(b, n->op, l, r);
    }

    if (l.typeDesc.isFloat && !r.typeDesc.isFloat)
    {
        r = Coerce(b, r, l.typeDesc);
    }
    else if (!l.typeDesc.isFloat && r.typeDesc.isFloat)
    {
        l = Coerce(b, l, r.typeDesc);

        typeDesc = r.typeDesc;
    }
    else if (l.typeDesc.isFloat && r.typeDesc.isFloat && l.typeDesc.type != r.typeDesc.type)
    {
        /* float mixed with double: compute in the wider type (fpext the float
           side) - the old code left the operands at mismatched widths, which
           emitted invalid IR like `fmul float X, double Y`. */
        LLVMTypeRef d = LLVMDoubleTypeInContext(b->m_ctx);

        if (l.typeDesc.type == d)
        {
            r = Coerce(b, r, l.typeDesc);
            typeDesc = l.typeDesc;
        }
        else
        {
            l = Coerce(b, l, r.typeDesc);
            typeDesc = r.typeDesc;
        }
    }
    else if (!l.typeDesc.isFloat && !r.typeDesc.isFloat && l.typeDesc.type != r.typeDesc.type && l.typeDesc.type
             && r.typeDesc.type)
    {
        /* Mixed integer widths: compute in the wider type (`int + byte`
           must not truncate the int to i8). Non-integer operands (pointers
           etc.) keep the old left-to-right coercion. */
        bool bothInt = LLVMGetTypeKind(l.typeDesc.type) == LLVMIntegerTypeKind
                       && LLVMGetTypeKind(r.typeDesc.type) == LLVMIntegerTypeKind;
        bool leftWider = bothInt ? LLVMGetIntTypeWidth(l.typeDesc.type) > LLVMGetIntTypeWidth(r.typeDesc.type)
                                 : l.typeDesc.type == I64Ty(b);

        if (leftWider)
        {
            r = Coerce(b, r, l.typeDesc);
        }
        else
        {
            l = Coerce(b, l, r.typeDesc);
            typeDesc = r.typeDesc;
        }
    }

    LLVMValueRef out = NULL;
    bool flt = typeDesc.isFloat;

    TypeDesc boolTypeDesc = TypeDescMake(I1Ty(b), 0, NULL);

    /* For comparisons involving pointer types (string, box, handle), convert
       both operands to i64 so that icmp works correctly. */
    if (typeDesc.type == b->m_ptrTy || r.typeDesc.type == b->m_ptrTy)
    {
        switch (n->op)
        {
        case BinEqEq:
        case BinNotEq:
        case BinLt:
        case BinLtEq:
        case BinGt:
        case BinGtEq:
            typeDesc = TypeDescMake(I64Ty(b), TD_UNSIGNED, NULL);
            flt = false;
            if (l.typeDesc.type == b->m_ptrTy)
            {
                l.value = LLVMBuildPtrToInt(b->m_builder, l.value, I64Ty(b), "picmp");
                l.typeDesc = typeDesc;
            }
            else
            {
                l = Coerce(b, l, typeDesc);
            }
            if (r.typeDesc.type == b->m_ptrTy)
            {
                r.value = LLVMBuildPtrToInt(b->m_builder, r.value, I64Ty(b), "picmp");
                r.typeDesc = typeDesc;
            }
            else
            {
                r = Coerce(b, r, typeDesc);
            }
            break;
        default:
            break;
        }
    }

    switch (n->op)
    {
    case BinAdd:
        out = flt ? LLVMBuildFAdd(b->m_builder, l.value, r.value, "add")
                  : LLVMBuildAdd(b->m_builder, l.value, r.value, "add");

        return ValueMake(out, typeDesc);

    case BinSub:
        out = flt ? LLVMBuildFSub(b->m_builder, l.value, r.value, "sub")
                  : LLVMBuildSub(b->m_builder, l.value, r.value, "sub");

        return ValueMake(out, typeDesc);

    case BinMul:
        out = flt ? LLVMBuildFMul(b->m_builder, l.value, r.value, "mul")
                  : LLVMBuildMul(b->m_builder, l.value, r.value, "mul");

        return ValueMake(out, typeDesc);

    case BinDiv:
        if (!flt)
        {
            EmitDivisionChecks(b, typeDesc, l.value, r.value, "integer division by zero", "integer division overflows");
        }

        out = flt ? LLVMBuildFDiv(b->m_builder, l.value, r.value, "div")
                  : (typeDesc.isUnsigned ? LLVMBuildUDiv(b->m_builder, l.value, r.value, "div")
                                         : LLVMBuildSDiv(b->m_builder, l.value, r.value, "div"));

        return ValueMake(out, typeDesc);

    case BinMod:
        if (!flt)
        {
            EmitDivisionChecks(b, typeDesc, l.value, r.value, "integer modulo by zero", "integer modulo overflows");
        }

        out = flt ? LLVMBuildFRem(b->m_builder, l.value, r.value, "mod")
                  : (typeDesc.isUnsigned ? LLVMBuildURem(b->m_builder, l.value, r.value, "mod")
                                         : LLVMBuildSRem(b->m_builder, l.value, r.value, "mod"));

        return ValueMake(out, typeDesc);

    case BinBitAnd:
        out = LLVMBuildAnd(b->m_builder, l.value, r.value, "and");

        return ValueMake(out, typeDesc);

    case BinBitOr:
        out = LLVMBuildOr(b->m_builder, l.value, r.value, "or");

        return ValueMake(out, typeDesc);

    case BinBitXor:
        out = LLVMBuildXor(b->m_builder, l.value, r.value, "xor");

        return ValueMake(out, typeDesc);

    case BinShl:
        EmitShiftCheck(b, l.value, r.value);
        out = LLVMBuildShl(b->m_builder, l.value, r.value, "shl");

        return ValueMake(out, typeDesc);

    case BinShr:
        EmitShiftCheck(b, l.value, r.value);
        out = typeDesc.isUnsigned ? LLVMBuildLShr(b->m_builder, l.value, r.value, "shr")
                                  : LLVMBuildAShr(b->m_builder, l.value, r.value, "shr");

        return ValueMake(out, typeDesc);

    case BinEqEq:
        out = flt ? FcmpByName(b->m_builder, "oeq", l.value, r.value)
                  : IcmpByName(b->m_builder, "eq", typeDesc.isUnsigned, l.value, r.value);

        return ValueMake(AllLanesTrue(b, out), boolTypeDesc);

    case BinNotEq:
        /* Vectors differ when they are not all-equal; scalars keep the ordered `one`. */
        if (flt && LLVMGetTypeKind(LLVMTypeOf(l.value)) == LLVMVectorTypeKind)
        {
            out = LLVMBuildNot(b->m_builder, AllLanesTrue(b, FcmpByName(b->m_builder, "oeq", l.value, r.value)), "vne");

            return ValueMake(out, boolTypeDesc);
        }

        out = flt ? FcmpByName(b->m_builder, "one", l.value, r.value)
                  : IcmpByName(b->m_builder, "ne", typeDesc.isUnsigned, l.value, r.value);

        return ValueMake(AllLanesTrue(b, out), boolTypeDesc);

    case BinLt:
        out = flt ? FcmpByName(b->m_builder, "olt", l.value, r.value)
                  : IcmpByName(b->m_builder, "lt", typeDesc.isUnsigned, l.value, r.value);

        return ValueMake(out, boolTypeDesc);

    case BinLtEq:
        out = flt ? FcmpByName(b->m_builder, "ole", l.value, r.value)
                  : IcmpByName(b->m_builder, "le", typeDesc.isUnsigned, l.value, r.value);

        return ValueMake(out, boolTypeDesc);

    case BinGt:
        out = flt ? FcmpByName(b->m_builder, "ogt", l.value, r.value)
                  : IcmpByName(b->m_builder, "gt", typeDesc.isUnsigned, l.value, r.value);

        return ValueMake(out, boolTypeDesc);

    case BinGtEq:
        out = flt ? FcmpByName(b->m_builder, "oge", l.value, r.value)
                  : IcmpByName(b->m_builder, "ge", typeDesc.isUnsigned, l.value, r.value);

        return ValueMake(out, boolTypeDesc);

    default:
        return l;
    }
}
