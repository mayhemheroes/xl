// *****************************************************************************
// compiler-expr.cpp                                                  XL project
// *****************************************************************************
//
// File description:
//
//    Compilation of XL expressions ("expression reduction")
//
//
//
//
//
//
//
//
// *****************************************************************************
// This software is licensed under the GNU General Public License v3+
// (C) 2010-2012,2014-2020, Christophe de Dinechin <christophe@dinechin.org>
// (C) 2012, Jérôme Forissier <jerome@taodyne.com>
// *****************************************************************************
// This file is part of XL
//
// XL is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License,
// or (at your option) any later version.
//
// XL is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with XL, in a file named COPYING.
// If not, see <https://www.gnu.org/licenses/>.
// *****************************************************************************

#include "compiler-expr.h"
#include "compiler-unit.h"
#include "compiler-rewrites.h"
#include "compiler-types.h"
#include "context.h"
#include "save.h"
#include "basics.h"
#include "errors.h"
#include "renderer.h"
#include "llvm-crap.h"


RECORDER(compiler_expr,   128, "Expression reduction (compilation of calls)");

XL_BEGIN


static bool RewriteBodyReferencesName(Tree *tree, const text &nm)
// ----------------------------------------------------------------------------
//   True if the rewrite RHS references this binding name as an identifier.
// ----------------------------------------------------------------------------
//   Pattern-only bindings must not be evaluated before the RHS (e.g. the
//   then-branch of [if [[false]] then TrueBody is false] is not executed).
{
    if (!tree)
        return false;
    if (Name *n = tree->AsName())
        return n->value == nm;
    if (Block *b = tree->AsBlock())
        return RewriteBodyReferencesName(b->child, nm);
    if (Prefix *p = tree->AsPrefix())
        return RewriteBodyReferencesName(p->left, nm) ||
               RewriteBodyReferencesName(p->right, nm);
    if (Infix *ix = tree->AsInfix())
        return RewriteBodyReferencesName(ix->left, nm) ||
               RewriteBodyReferencesName(ix->right, nm);
    if (Postfix *px = tree->AsPostfix())
        return RewriteBodyReferencesName(px->left, nm) ||
               RewriteBodyReferencesName(px->right, nm);
    return false;
}


static JIT::Value_p UnusedRewriteBindingValue(JITBlock &code, JIT::Type_p ty)
// ----------------------------------------------------------------------------
//   Placeholder value for a binding not referenced by the rewrite RHS.
// ----------------------------------------------------------------------------
//   The compiled body must not load this argument; type matches the signature.
{
    return code.NullConstant(ty);
}


// ============================================================================
//
//    Compile an expression
//
// ============================================================================

CompilerExpression::CompilerExpression(CompilerFunction &function)
// ----------------------------------------------------------------------------
//   Constructor for a compiler expression
// ----------------------------------------------------------------------------
    : CompilerExpression(function, function.FunctionContext())
{}


CompilerExpression::CompilerExpression(CompilerFunction &function,
                                       Context          *context)
// ----------------------------------------------------------------------------
//   Constructor for a compiler expression
// ----------------------------------------------------------------------------
    : function(function),
      computed(),
      context(context)
{}


JIT::Value_p CompilerExpression::Evaluate(Tree *expr, bool force)
// ----------------------------------------------------------------------------
//   For top-level expressions, make sure we evaluate closures
// ----------------------------------------------------------------------------
{
    JIT::Value_p result = expr->Do(this);
    if (result)
    {
        CompilerUnit &unit = function.unit;
        JITBlock &code = function.code;
        JIT::Type_p mtype = code.Type(result);
        function.ValueMachineType(expr, mtype);
        if (force && unit.IsClosureType(mtype))
        {
            result = function.InvokeThickClosure(expr, result, mtype);
            mtype = code.Type(result);
            function.ValueMachineType(expr, mtype);
        }
    }
    return result;
}


JIT::Value_p CompilerExpression::Do(Natural *what)
// ----------------------------------------------------------------------------
//   Compile an natural constant
// ----------------------------------------------------------------------------
{
    Compiler &compiler = function.compiler;
    JITBlock &code = function.code;
    return code.IntegerConstant(compiler.naturalTy, int64_t(what->value));
}


JIT::Value_p CompilerExpression::Do(Real *what)
// ----------------------------------------------------------------------------
//   Compile a real constant
// ----------------------------------------------------------------------------
{
    Compiler &compiler = function.compiler;
    JITBlock &code = function.code;
    return code.FloatConstant(compiler.realTy, what->value);
}


JIT::Value_p CompilerExpression::Do(Text *what)
// ----------------------------------------------------------------------------
//   Compile a text constant
// ----------------------------------------------------------------------------
{
    Compiler &compiler = function.compiler;
    JITBlock &code = function.code;
    if (what->IsCharacter())
    {
        char c = what->value.length() ? what->value[0] : 0;
        return code.IntegerConstant(compiler.characterTy, c);
    }
    return code.TextConstant(compiler.charPtrTy, what->value);
}


JIT::Value_p CompilerExpression::Do(Name *what)
// ----------------------------------------------------------------------------
//   Compile a name
// ----------------------------------------------------------------------------
{
    CompilerUnit &unit     = function.unit;
    JITBlock     &code     = function.code;
    Scope_g       where;
    Rewrite_g     rewrite;
    Tree         *existing = context->Bound(what, true, &rewrite, &where);
    assert(existing || !"Type checking didn't realize a name is missing");
    Tree *from = PatternBase(rewrite->left);
    if (where == context->Symbols())
    {
        if (JIT::Value_p result = function.Known(from))
            return result;
    }

    // Check true and false values
    if (existing == xl_true)
        return code.BooleanConstant(true);
    if (existing == xl_false)
        return code.BooleanConstant(false);

    // Check if it is a global
    if (JIT::Value_p global = unit.Global(existing))
        return global;
    if (JIT::Value_p global = unit.Global(from))
        return global;

    JIT::Value_p result = DoCall(what, true);
    if (!result)
        result = Value(existing);

    return result;
}


JIT::Value_p CompilerExpression::Do(Infix *infix)
// ----------------------------------------------------------------------------
//   Compile infix expressions
// ----------------------------------------------------------------------------
{
    // Sequences
    if (IsSequence(infix))
    {
        JIT::Value_p left = Evaluate(infix->left, true);
        JIT::Value_p right = Evaluate(infix->right, true);
        if (right)
            return right;
        if (left)
            return left;
        return function.ConstantTree(xl_nil);
    }

    // Type casts - REVISIT: may need to do some actual conversion
    if (IsTypeAnnotation(infix))
        return infix->left->Do(this);

    // Declarations: it's too early to define a function just yet,
    // because we don't have the actual argument types.
    if (IsDefinition(infix))
        return function.ConstantTree(infix);

    // Assignment
    if (IsAssignment(infix))
        return DoAssignment(infix);

    // General case: expression
    return DoCall(infix);
}


JIT::Value_p CompilerExpression::Do(Prefix *what)
// ----------------------------------------------------------------------------
//   Compile prefix expressions
// ----------------------------------------------------------------------------
{
    if (Name *name = what->left->AsName())
    {
        if (name->value == "data" || name->value == "extern")
            return function.ConstantTree(what);

        if (name->value == "builtin")
        {
            // This is a builtin, find if we write to code or data
            Tree *builtin = what->right;
            Name *name = builtin->AsName();
            if (!name)
            {
                Ooops("Malformed primitive $1", name);
                return function.ConstantTree(builtin);
            }

            // Take args list for current function as input
            JIT::Values args;
            JIT::Function_p fn = function.Function();
            JITArguments inputs(fn);
            for (size_t i = 0; i < inputs.Count(); i++)
            {
                JIT::Value_p input = *inputs++;
                args.push_back(input);
            }

            // Call the primitive (effectively creating a wrapper for it)
            text op = name->value;
            uint sz = args.size();
            JIT::Value_p *a = &args[0];
            return function.Primitive(what, op, sz, a);
        }
    }

    return DoCall(what);
}


JIT::Value_p CompilerExpression::Do(Postfix *what)
// ----------------------------------------------------------------------------
//   Compile postfix expressions
// ----------------------------------------------------------------------------
{
    return DoCall(what);
}


JIT::Value_p CompilerExpression::Do(Block *block)
// ----------------------------------------------------------------------------
//   Compile blocks
// ----------------------------------------------------------------------------
{
    return block->child->Do(this);
}


JIT::Value_p CompilerExpression::DoCall(Tree *call, bool mayfail)
// ----------------------------------------------------------------------------
//   Compile expressions into calls for the right expression
// ----------------------------------------------------------------------------
{
    JIT::Value_p result = nullptr;

    record(compiler_expr, "Call %t", call);
    CompilerTypes *types = function.types;
    CompilerRewriteCalls *rc = types->TreeRewriteCalls(call);
    if (!rc)
    {
        // Top-level TypeAnalysis may not have walked nested rewrite bodies
        // (e.g. builtins). Infer the call here so rcalls exist for codegen.
        types->Type(call);
        rc = types->TreeRewriteCalls(call);
    }
    record(types_calls, "Looking up %t in %p: got %p", call, types, rc);
    if (mayfail && !rc)
        return nullptr;
    if (!rc)
    {
        Ooops("No operator matches $1", call);
        return nullptr;
    }

    // Optimize the frequent case where we have a single call candidate
    uint i, max = rc->Size();
    record(compiler_expr, "Call %t has %u candidates", call, max);
    if (max == 1)
    {
        // We now evaluate in that rewrite's type system
        CompilerRewriteCandidate* cand = rc->Candidate(0);
        if (cand->Unconditional())
        {
            result = DoRewrite(call, (CompilerRewriteCandidate *) cand);
            return result;
        }
    }
    else if (max == 0)
    {
        // If it passed type check and there is no candidate, return tree as is
        result = function.BoxedTree(call);
        return result;
    }
    // More general case: we need to generate expression reduction
    JITBlock &code = function.code;
    JITBlock isDone(code, "done");
    JIT::Type_p storageType = function.ValueMachineType(call);
    JIT::Value_p storage = function.NeedStorage(call, storageType);
    Compiler &compiler = function.compiler;

    for (i = 0; i < max; i++)
    {
        // Now evaluate in that candidate's type system
        CompilerRewriteCandidate *cand = rc->Candidate(i);
        JIT::Value_p condition = nullptr;

        // Perform tree-kind tests to check if this candidate is valid
        for (RewriteTypeCheck &tc : cand->typechecks)
        {
            JIT::Value_p value = Value(tc.value);
            value = function.Autobox(tc.value, value, compiler.treePtrTy);
            JIT::Value_p cast = function.CallTypeCheck(tc.type, value);
            JIT::Value_p compare = code.IsNullPointer(cast, "cast");
            record(compiler_expr, "Type test for %t for value %t type %t: %v",
                   call, tc.value, tc.type, compare);
            if (condition)
                condition = code.And(condition, compare);
            else
                condition = compare;
        } // Types

        // Perform the tests to check if this candidate is valid
        for (RewriteCondition &t : cand->conditions)
        {
            JIT::Value_p compare = Compare(t.value, t.test);
            record(compiler_expr, "Condition test for %t candidate %u: %v",
                   call, i, compare);
            if (condition)
                condition = code.And(condition, compare);
            else
                condition = compare;
        }

        if (condition)
        {
            JITBlock isBad(code, "bad");
            JITBlock isGood(code, "good");
            code.IfBranch(condition, isGood, isBad);
            code.SwitchTo(isGood);
            value_map saveComputed = computed;

            // REVISIT: Insert cast of types here

            result = DoRewrite(call, (CompilerRewriteCandidate *) cand);
            computed = saveComputed;
            result = function.Autobox(call, result, storageType);
            record(compiler_expr, "Call %t candidate %u is conditional: %v",
                   call, i, result);
            code.Store(result, storage);
            code.Branch(isDone);
            code.SwitchTo(isBad);
        }
        else
        {
            // If this particular call was unconditional, we are done
            result = DoRewrite(call, (CompilerRewriteCandidate *) cand);
            result = function.Autobox(call, result, storageType);
            code.Store(result, storage);
            code.Branch(isDone);
            code.SwitchTo(isDone);
            result = code.Load(storageType, storage);
            return result;
        }
    }

    // The final call to xl_form_error if nothing worked
    function.CallFormError(call);
    code.Branch(isDone);
    code.SwitchTo(isDone);
    result = code.Load(storageType, storage);
    record(compiler_expr, "No match for call %t, inserted form error: %v",
           call, result);
    return result;
}


JIT::Value_p CompilerExpression::DoRewrite(Tree *call,
                                           CompilerRewriteCandidate *cand)
// ----------------------------------------------------------------------------
//   Generate code for a particular rewwrite candidate
// ----------------------------------------------------------------------------
{
    Rewrite *rw = cand->rewrite;
    JIT::Value_p result = nullptr;
    JITBlock &code = function.code;

    record(compiler_expr, "Rewrite: %t", rw);

    // Evaluate parameters
    JIT::Values args;
    RewriteBindings &bnds = cand->bindings;
    CompilerTypes   *btypes = (CompilerTypes *) cand->BindingTypes();
    CompilerTypes   *vtypes = (CompilerTypes *) cand->ValueTypes();
    CompilerTypes::Decl rwcat = CompilerTypes::RewriteCategory(cand);
    bool lazyBindings = (rwcat == CompilerTypes::Decl::NORMAL);
    Tree *rhs = rw->right;
    for (RewriteBinding &b : bnds)
    {
        Tree        *arg   = b.value;
        JIT::Value_p value = nullptr;

        if (lazyBindings && rhs && b.name &&
            !RewriteBodyReferencesName(rhs, b.name->value))
        {
            JIT::Type_p mtype = function.ValueMachineType(arg);
            value = UnusedRewriteBindingValue(code, mtype);
            record(compiler_expr,
                   "Rewrite %t: skip eager eval for unused binding %t",
                   rw, b.name);
        }
        else
        {
            value = Value(arg);
        }
        args.push_back(value);

        Tree       *argtype = vtypes->ValueType(arg);
        JIT::Type_p mtype   = function.ValueMachineType(arg);
        if (!btypes->IsPatternType(argtype))
            btypes->AddBoxedType(argtype, mtype);

        record(compiler_expr, "Rewrite %t arg %t value %v", rw, arg, value);
    }

    // Check if this is an LLVM builtin
    Tree *builtin = nullptr;
    if (Tree *value = rw->right)
        if (Prefix *prefix = value->AsPrefix())
            if (Name *name = prefix->left->AsName())
                if (name->value == "builtin")
                    builtin = prefix->right;

    if (builtin)
    {
        record(compiler_expr, "Rewrite %t is builtin %t", rw, builtin);
        Name *name = builtin->AsName();
        if (!name)
        {
            Ooops("Malformed primitive $1", builtin);
            result = function.CallFormError(builtin);
            record(compiler_expr,
                   "Rewrite %t is malformed builtin %t: form error %v",
                   rw, builtin, result);
        }
        else
        {
            text op = name->value;
            size_t sz = args.size();
            JIT::Value_p *a = &args[0];
            result = function.Primitive(builtin, op, sz, a);
            record(compiler_expr, "Rewrite %t is builtin %t: %v",
                   rw, builtin, result);
        }
    }
    else if (JIT::Function_p fn = function.Compile(call, cand, args))
    {
        result = code.Call(fn, args);
        record(compiler_expr, "Rewrite %t function %v call %v",
               rw, fn, result);
    }
    else
    {
        record(compiler_error,
               "Could not compile %t for rewrite %t", call, rw);

    }

    // Save the type of the return value
    if (result)
    {
        CompilerTypes *vtypes = cand->ValueTypes();
        Tree *base = vtypes->CodeGenerationType(call);
        JIT::Type_p retTy = code.Type(result);
        if (!vtypes->IsPatternType(base))
            function.AddBoxedType(base, retTy);
        record(compiler_expr, "Transporting type %t (%T) of %t into %p",
               base, retTy, call, vtypes);
    }

    return result;
}


JIT::Value_p CompilerExpression::DoAssignment(Infix *assign)
// ----------------------------------------------------------------------------
//   Store into the storage for the value being assigned to
// ----------------------------------------------------------------------------
{
    Scope_g       where;
    Rewrite_g     rw;
    JITBlock     &code        = function.code;
    Tree         *existing    = context->Bound(assign->left, true, &rw, &where);
    JIT::Type_p   storageType = function.ValueMachineType(existing);
    JIT::Value_p  storage     = function.NeedStorage(existing, storageType);
    JIT::Value_p  value       = Evaluate(assign->right);
    return code.Store(storage, value);
}


JIT::Value_p CompilerExpression::Value(Tree *expr)
// ----------------------------------------------------------------------------
//   Evaluate an expression once
// ----------------------------------------------------------------------------
{
    JIT::Value_p value = computed[expr];
    if (!value)
    {
        value = Evaluate(expr);
        computed[expr] = value;
    }
    return value;
}


JIT::Value_p CompilerExpression::Compare(Tree *valueTree, Tree *testTree)
// ----------------------------------------------------------------------------
//   Perform a comparison between the two values and check if this matches
// ----------------------------------------------------------------------------
{
    JITBlock     &code      = function.code;
    Compiler     &compiler  = function.compiler;
    CompilerUnit &unit      = function.unit;

    if (Name *vt = valueTree->AsName())
        if (Name *tt = testTree->AsName())
            if (vt->value == tt->value)
                return code.BooleanConstant(true);

    JIT::Value_p value     = Value(valueTree);
    JIT::Value_p test      = Value(testTree);
    JIT::Type_p  valueType = code.Type(value);
    JIT::Type_p  testType  = code.Type(test);


    // Comparison of boolean values
    if (testType == compiler.booleanTy)
    {
        if (valueType == compiler.treePtrTy ||
            valueType == compiler.nameTreePtrTy)
        {
            value = function.Autobox(valueTree, value, compiler.booleanTy);
            valueType = code.Type(value);
        }
        if (valueType != compiler.booleanTy)
            return code.BooleanConstant(false);
        return code.ICmpEQ(test, value);
    }

    // Comparison of character values
    if (testType == compiler.characterTy)
    {
        if (valueType == compiler.textTreePtrTy)
        {
            value = function.Autobox(valueTree, value, testType);
            valueType = code.Type(value);
        }
        if (valueType != compiler.characterTy)
            return code.BooleanConstant(false);
        return code.ICmpEQ(test, value);
    }

    // Comparison of text constants
    if (testType == compiler.textTy || testType == compiler.textPtrTy)
    {
        test = function.Autobox(testTree, test, compiler.charPtrTy);
        testType = test->getType();
    }
    if (testType == compiler.charPtrTy)
    {
        if (valueType == compiler.textTreePtrTy)
        {
            value = function.Autobox(valueTree, value, testType);
            valueType = code.Type(value);
        }
        if (valueType != compiler.charPtrTy)
            return code.BooleanConstant(false);
        value = code.Call(unit.strcmp, test, value);
        test = code.IntegerConstant(code.Type(value), 0);
        value = code.ICmpEQ(value, test);
        return value;
    }

    // Comparison of natural values
    if (testType->isIntegerTy())
    {
        if (valueType == compiler.naturalTreePtrTy)
        {
            value = function.Autobox(valueTree, value, compiler.naturalTy);
            valueType = code.Type(value);
        }
        if (!valueType->isIntegerTy())
            return code.BooleanConstant(false);
        if (valueType != testType)
            value = code.BitCast(value, testType);
        return code.ICmpEQ(test, value);
    }

    // Comparison of floating-point values
    if (testType->isFloatingPointTy())
    {
        if (valueType == compiler.realTreePtrTy)
        {
            value = function.Autobox(valueTree, value, compiler.realTy);
            valueType = code.Type(value);
        }
        if (!valueType->isFloatingPointTy())
            return code.BooleanConstant(false);
        if (valueType != testType)
        {
            if (valueType != compiler.realTy)
            {
                value = code.FPExt(value, compiler.realTy);
                valueType = code.Type(value);
            }
            if (testType != compiler.realTy)
            {
                test = code.FPExt(test, compiler.realTy);
                testType = test->getType();
            }
            if (valueType != testType)
                return code.BooleanConstant(false);
        }
        return code.FCmpOEQ(test, value);
    }

    // Test our special types
    if (testType == compiler.treePtrTy         ||
        testType == compiler.naturalTreePtrTy  ||
        testType == compiler.realTreePtrTy     ||
        testType == compiler.textTreePtrTy     ||
        testType == compiler.nameTreePtrTy     ||
        testType == compiler.blockTreePtrTy    ||
        testType == compiler.infixTreePtrTy    ||
        testType == compiler.prefixTreePtrTy   ||
        testType == compiler.postfixTreePtrTy)
    {
        if (testType != compiler.treePtrTy)
        {
            test = code.BitCast(test, compiler.treePtrTy);
            testType = test->getType();
        }

        // Convert value to a Tree * if possible
        if (valueType->isIntegerTy()                    ||
            valueType->isFloatingPointTy()              ||
            valueType == compiler.charPtrTy             ||
            valueType == compiler.textTy                ||
            valueType == compiler.textPtrTy             ||
            valueType == compiler.naturalTreePtrTy      ||
            valueType == compiler.realTreePtrTy         ||
            valueType == compiler.textTreePtrTy         ||
            valueType == compiler.nameTreePtrTy         ||
            valueType == compiler.blockTreePtrTy        ||
            valueType == compiler.infixTreePtrTy        ||
            valueType == compiler.prefixTreePtrTy       ||
            valueType == compiler.postfixTreePtrTy)
        {
            value = function.Autobox(valueTree, value, compiler.treePtrTy);
            valueType = code.Type(value);
        }

        if (testType != valueType)
            return code.BooleanConstant(false);

        // Call runtime function to perform tree comparison
        return code.Call(unit.xl_same_shape, value, test);
    }

    // Other comparisons fail for now
    return code.BooleanConstant(false);
}

XL_END
