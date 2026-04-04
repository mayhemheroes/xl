#ifndef COMPILER_FUNCTION_H
#define COMPILER_FUNCTION_H
// *****************************************************************************
// compiler-function.h                                                XL project
// *****************************************************************************
//
// File description:
//
//     A function generated in a CompilerUnit
//
//     There are, broadly, two kinds of functions being generated:
//     1. CompilerEval functions, with evalTy as their signature (aka eval_fn)
//     2. CompilerFunction Optimized functions, with arbitrary signatures.
//
//     Optimized functions have a 'closure type' as their first argument
//     if symbols from surrounding contexts were captured during analysis
//
//
// *****************************************************************************
// This software is licensed under the GNU General Public License v3+
// (C) 2018-2020, Christophe de Dinechin <christophe@dinechin.org>
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

#include "compiler.h"
#include "compiler-unit.h"
#include "compiler-rewrites.h"
#include "compiler-prototype.h"
#include "compiler-types.h"


XL_BEGIN

class CompilerFunction : public CompilerPrototype
// ----------------------------------------------------------------------------
//    A function generated in a compile unit
// ----------------------------------------------------------------------------
{
protected:
    Compiler &          compiler;   // The compiler environment we use
    JIT &               jit;        // The JIT compiler (LLVM API stabilizer)
    Tree_g              body;       // Body for this function
    JITBlock            data;       // A basic block for local variables
    JITBlock            code;       // A basic block for current code
    JITBlock            exit;       // A basic block for shared exit
    JIT::BasicBlock_g   entry;      // The entry point for the function code
    JIT::Value_g        returned;   // Returned value
    JIT::Type_g         closure;    // Closure type if any
    value_map           values;     // Tree -> LLVM value
    value_map           storage;    // Tree -> LLVM storage (alloca)

    friend class CompilerExpression;

public:
    // Constructors for the top-level functions
    CompilerFunction(CompilerUnit &unit,
                     Tree *pattern,
                     Tree *body,
                     CompilerTypes *types,
                     JIT::FunctionType_g ftype,
                     text name);
    CompilerFunction(CompilerFunction &caller, CompilerRewriteCandidate *rc);
    ~CompilerFunction();

    bool                IsInterfaceOnly() override;


    JIT::Function_g     Compile(Tree *tree, bool forceEvaluation = false);
    JIT::Value_g        Return(Tree *tree, JIT::Value_g value);
    eval_fn             Finalize(bool createCode);

    JIT::Type_g         ValueMachineType(Tree *expr, bool mayfail = false);
    void                ValueMachineType(Tree *expr, JIT::Type_g type);
    JIT::Type_g         BoxedType(Tree *type);

private:
    // Function interface creation
    void                InitializeArgs();
    void                InitializeArgs(CompilerRewriteCandidate *rc);

    // Machine types management
    void                AddBoxedType(Tree *treeType, JIT::Type_g machineType);
    JIT::Type_g         HasBoxedType(Tree *type);

    JIT::Type_g         ReturnType(Tree *pattern);
    JIT::Type_g         StructureType(Tree *rwform, Tree *type);
    JIT::Type_g         StructureType(const JIT::Signature &signature,
                                      Tree *rwform, Tree *type);
    JIT::Value_g        BoxedTree(Tree *what);
    void                BoxedTreeType(JIT::Signature &sig, Tree *what);

private:
    // Compilation of rewrites and data
    JIT::Value_g        Compile(Tree *call,
                                CompilerRewriteCandidate *rc,
                                const JIT::Values &args);
    JIT::Value_g        Data(Tree        *pattern,
                             JIT::Value_g box,
                             JIT::Type_g  boxTy,
                             unsigned    &index);
    JIT::Value_g        Autobox(Tree        *source,
                                JIT::Value_g value,
                                JIT::Type_g  requested);
    JIT::Function_g     UnboxFunction(JIT::Type_g type, Tree *pattern);
    JIT::Value_g        Unbox(JIT::Value_g arg,
                              JIT::Type_g  type,
                              Tree        *pattern,
                              uint        &index);
    JIT::Value_g        Primitive(Tree *,
                                  text name,
                                  uint arity,
                                  JIT::Value_g *args);

    // Storage management
    enum { knowAll = -1, knowGlobals = 1, knowLocals = 2, knowValues = 4 };
    JIT::Value_g        NeedStorage(Tree *tree, JIT::Type_g ty = nullptr);
    bool                IsKnown(Tree *tree, uint which = knowAll);
    JIT::Value_g        Known(Tree *tree, uint which = knowAll );

    // Creating constants
    JIT::Value_g        ConstantNatural(Natural *what);
    JIT::Value_g        ConstantReal(Real *what);
    JIT::Value_g        ConstantText(Text *what);
    JIT::Value_g        ConstantTree(Tree *what);

    // Error management
    JIT::Value_g        CallFormError(Tree *what);
    JIT::Value_g        CallTypeCheck(Tree *type, JIT::Value_g value);

protected:
    // Primitives, i.e. functions generating native LLVM code
    typedef JIT::Value_g (CompilerFunction::*primitive_fn)(Tree *,
                                                           JIT::Value_g *args);
    typedef JIT::Type_g  (CompilerFunction::*mtype_fn)(Tree *);
    struct PrimitiveInfo
    {
        primitive_fn    function;
        unsigned        arity;
    };
    struct MachineTypeInfo
    {
        mtype_fn        function;
        unsigned        arity;
    };
    typedef std::map<text,PrimitiveInfo>        Primitives;
    typedef std::map<text,MachineTypeInfo>      MachineTypes;
    static Primitives   primitives;
    static MachineTypes mtypes;

    static void         InitializePrimitives();

    // Define LLVM accessors for primitives
#define MTYPE(Name, Arity, Code)                                        \
    JIT::Type_g         llvm_type_##Name(Tree *source);
#define UNARY(Name)                                                     \
    JIT::Value_g        llvm_##Name(Tree *source, JIT::Value_g *args);
#define BINARY(Name)                                                    \
    JIT::Value_g        llvm_##Name(Tree *source, JIT::Value_g *args);
#define CAST(Name)                                                      \
    JIT::Value_g        llvm_##Name(Tree *source, JIT::Value_g *args);
#define SPECIAL(Name, Arity, Code)                                      \
    JIT::Value_g        llvm_##Name(Tree *source, JIT::Value_g *args);

#define ALIAS(from, arity, to)
#define EXTERNAL(Name, ...)

#include "compiler-primitives.tbl"
};


class CompilerEval : public CompilerFunction
// ----------------------------------------------------------------------------
//   A compiler eval function
// ----------------------------------------------------------------------------
{
public:
    CompilerEval(CompilerUnit &unit,
                 Tree *body,
                 CompilerTypes *types);

};

XL_END

RECORDER_DECLARE(compiler_function);
RECORDER_DECLARE(parameter_bindings);

#endif // COMPILER_FUNCTION_H
