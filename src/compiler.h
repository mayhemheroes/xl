#ifndef COMPILER_H
#define COMPILER_H
// *****************************************************************************
// compiler.h                                                         XL project
// *****************************************************************************
//
// File description:
//
//    Just-in-time compiler for the trees
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
// (C) 2009-2020, Christophe de Dinechin <christophe@dinechin.org>
// (C) 2010,2012, Jérôme Forissier <jerome@taodyne.com>
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

#include "tree.h"
#include "context.h"
#include "evaluator.h"
#include "llvm-crap.h"
#include <map>
#include <set>



// ============================================================================
//
//    Forward declarations
//
// ============================================================================

RECORDER_DECLARE(compiler);
RECORDER_DECLARE(compiler_warning);
RECORDER_DECLARE(compiler_error);

XL_BEGIN
// ============================================================================
//
//    Global structures to access the LLVM just-in-time compiler
//
// ============================================================================

struct Compiler : Evaluator
// ----------------------------------------------------------------------------
//   Just-in-time compiler data
// ----------------------------------------------------------------------------
{
    Compiler(kstring name, unsigned opts, int argc, char **argv);
    ~Compiler();

    // Interpreter interface
    Tree *              Evaluate(Scope *, Tree *source) override;
    Tree *              TypeCheck(Scope *, Tree *type, Tree *val) override;

    // Find the machine type corresponding to the tree type or value
    JIT::PointerType_g  TreeMachineType(Tree *tree);
    JIT::Type_g         MachineType(Tree *tree);
    void                RebindTypesToJITContext();

public:
    JIT                 jit;
    JIT::Type_g         voidTy;
    JIT::IntegerType_g  booleanTy;
    JIT::IntegerType_g  naturalTy;
    JIT::IntegerType_g  natural8Ty;
    JIT::IntegerType_g  natural16Ty;
    JIT::IntegerType_g  natural32Ty;
    JIT::IntegerType_g  natural64Ty;
    JIT::IntegerType_g  natural128Ty;
    JIT::IntegerType_g  unsignedTy;
    JIT::IntegerType_g  ulongTy;
    JIT::IntegerType_g  ulonglongTy;
    JIT::Type_g         realTy;
    JIT::Type_g         real16Ty;
    JIT::Type_g         real32Ty;
    JIT::Type_g         real64Ty;
    JIT::IntegerType_g  characterTy;
    JIT::PointerType_g  charPtrTy;
    JIT::PointerType_g  charPtrPtrTy;
    JIT::StructType_g   textTy;
    JIT::PointerType_g  textPtrTy;
    JIT::StructType_g   infoTy;
    JIT::PointerType_g  infoPtrTy;
    JIT::StructType_g   treeTy;
    JIT::PointerType_g  treePtrTy;
    JIT::PointerType_g  treePtrPtrTy;
    JIT::StructType_g   naturalTreeTy;
    JIT::PointerType_g  naturalTreePtrTy;
    JIT::StructType_g   realTreeTy;
    JIT::PointerType_g  realTreePtrTy;
    JIT::StructType_g   textTreeTy;
    JIT::PointerType_g  textTreePtrTy;
    JIT::StructType_g   nameTreeTy;
    JIT::PointerType_g  nameTreePtrTy;
    JIT::StructType_g   blockTreeTy;
    JIT::PointerType_g  blockTreePtrTy;
    JIT::StructType_g   prefixTreeTy;
    JIT::PointerType_g  prefixTreePtrTy;
    JIT::StructType_g   postfixTreeTy;
    JIT::PointerType_g  postfixTreePtrTy;
    JIT::StructType_g   infixTreeTy;
    JIT::PointerType_g  infixTreePtrTy;
    JIT::StructType_g   scopeTy;
    JIT::PointerType_g  scopePtrTy;
    JIT::FunctionType_g evalTy;
    JIT::PointerType_g  evalFnTy;

    bool                IsTreeType(JIT::Type_g ty) const
    {
        return (ty == treeTy            ||
                ty == naturalTreeTy     ||
                ty == realTreeTy        ||
                ty == textTreeTy        ||
                ty == nameTreeTy        ||
                ty == blockTreeTy       ||
                ty == prefixTreeTy      ||
                ty == postfixTreeTy     ||
                ty == infixTreeTy);
    }
    bool                IsTreePointerType(JIT::Type_g ty) const
    {
        return (ty == treePtrTy         ||
                ty == naturalTreePtrTy  ||
                ty == realTreePtrTy     ||
                ty == textTreePtrTy     ||
                ty == nameTreePtrTy     ||
                ty == blockTreePtrTy    ||
                ty == prefixTreePtrTy   ||
                ty == postfixTreePtrTy  ||
                ty == infixTreePtrTy);
    }
};



// ============================================================================
//
//   Useful macros
//
// ============================================================================

// Index in data structures of fields in Tree types
#define TAG_INDEX           0
#define INFO_INDEX          1
#define NATURAL_VALUE_INDEX 2
#define REAL_VALUE_INDEX    2
#define TEXT_VALUE_INDEX    2
#define TEXT_OPENING_INDEX  3
#define TEXT_CLOSING_INDEX  4
#define NAME_VALUE_INDEX    2
#define BLOCK_CHILD_INDEX   2
#define BLOCK_OPENING_INDEX 3
#define BLOCK_CLOSING_INDEX 4
#define LEFT_VALUE_INDEX    2
#define RIGHT_VALUE_INDEX   3
#define INFIX_NAME_INDEX    4

XL_END

#endif // COMPILER_H
