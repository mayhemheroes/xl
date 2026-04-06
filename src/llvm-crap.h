#ifndef LLVM_CRAP_H
#define LLVM_CRAP_H
// *****************************************************************************
// llvm-crap.h                                                        XL project
// *****************************************************************************
//
// File description:
//
//     LLVM Compatibility Recovery Adaptive Protocol
//
//     LLVM keeps breaking the API from release to release.
//     Of course, none of the choices are documented anywhere, and
//     the documentation is left out of date for years.
//
//     So this file is an attempt at reverse engineering all the API
//     changes over time to be able to compile with various versions of LLVM
//     Be prepared for the worst. It's so ugly I had to dispose of it by
//     putting it in its own separate file.
//
//
// *****************************************************************************
// This software is licensed under the GNU General Public License v3+
// (C) 2014-2015,2017-2020, Christophe de Dinechin <christophe@dinechin.org>
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

#ifdef INTERPRETER_ONLY

// ============================================================================
//
//   Interpreter only stub types
//
// ============================================================================

namespace XL { namespace JIT {
typedef struct Interpreter_Type *       Type_g;
typedef struct Interpreter_Value *      Value_g;
}}



#else // !INTERPRETER_ONLY

// ============================================================================
//
//    Most basic LLVM headers required for all
//
// ============================================================================

#ifndef LLVM_VERSION
#error "Sorry, no can do anything without knowing the LLVM version"
#elif LLVM_VERSION < 370
// At some point, I have only so much time to waste on this.
// Feel free to enhance if you care about earlier versions of LLVM.
#error "LLVM 3.6 and earlier are not supported in this code."
#endif // LLVM_VERSION (see #ifndef LLVM_VERSION / #elif above)

#define LLVM_CRAP_DIAPER_OPEN
#include "llvm-crap.h"

// Fortunately, these LLVM headers are sufficient for our interface,
// and remained somewhat consistent across versions. Lucky us!
#include <llvm/IR/Type.h>
#include <llvm/IR/Constant.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Function.h>

#include <recorder/recorder.h>
#include <string>

#define LLVM_CRAP_DIAPER_CLOSE
#    include "llvm-crap.h"


// ============================================================================
//
//   LLVM data types
//
// ============================================================================

namespace llvm {
class Type;
class IntegerType;
class PointerType;
class ArrayType;
class StructType;
class FunctionType;

class Module;
class Function;
class BasicBlock;
class Value;
class GlobalValue;
class GlobalVariable;
class Constant;
}

typedef const char *            kstring;
typedef std::string             text;



// ============================================================================
//
//   Recorders related to LLVM
//
// ============================================================================

RECORDER_DECLARE(llvm);
RECORDER_DECLARE(llvm_prototypes);
RECORDER_DECLARE(llvm_externals);
RECORDER_DECLARE(llvm_functions);
RECORDER_DECLARE(llvm_constants);
RECORDER_DECLARE(llvm_builtins);
RECORDER_DECLARE(llvm_globals);
RECORDER_DECLARE(llvm_blocks);
RECORDER_DECLARE(llvm_labels);
RECORDER_DECLARE(llvm_calls);
RECORDER_DECLARE(llvm_stats);
RECORDER_DECLARE(llvm_code);
RECORDER_DECLARE(llvm_gc);
RECORDER_DECLARE(llvm_ir);



// ============================================================================
//
//   New ORC-based JIT support for after LLVM 5.0
//
// ============================================================================

namespace XL
{

struct Compiler;

class JIT
// ----------------------------------------------------------------------------
//  Interface to the LLVM JIT
// ----------------------------------------------------------------------------
//  The LLVM C++ interface keeps changing
//  This interface attempts to abstract away all these changes
//  This version is for ORC, the latest iteration of the LLVM JIT
{
    friend class JITBlock;
    friend class JITBlockPrivate;
    class JITPrivate &p;

public:
    // LLVM data types required for the JIT interface
    typedef llvm::Type                  *Type_g;
    typedef llvm::IntegerType           *IntegerType_g;
    typedef llvm::Type                  *PointerType_g;
    typedef llvm::ArrayType             *ArrayType_g;
    typedef llvm::StructType            *StructType_g;
    typedef llvm::FunctionType          *FunctionType_g;

    typedef llvm::Module                *Module_g;
    typedef llvm::Function              *Function_g;
    typedef llvm::BasicBlock            *BasicBlock_g;
    typedef llvm::Value                 *Value_g;
    typedef llvm::GlobalValue           *GlobalValue_g;
    typedef llvm::GlobalVariable        *GlobalVariable_g;
    typedef llvm::Constant              *Constant_g;

    typedef std::vector<Type_g>         Signature;
    typedef std::vector<Value_g>        Values;

    typedef intptr_t                    ModuleID;

public:
    enum { BitsPerByte = 8 };

public:
    JIT(int argc, char **argv);
    ~JIT();

public:
    static Type_g       Type(Value_g value);
    static Type_g       ReturnType(Function_g fn);
    static Type_g       PointedType(Type_g ptrt);
    static bool         IsStructType(Type_g strt);

    static bool         InUse(Function_g f);
    static void         EraseFromParent(Function_g f);

    static bool         VerifyFunction(Function_g function);
    static void         Print(kstring label, Value_g value);
    static void         Print(kstring label, Type_g type);
    static void         Comment(kstring comment);

public:
    // JIT attributes
    void                SetOptimizationLevel(unsigned opt);
    void                PrintStatistics();
    void                PrintCode();
    static void         StackTrace();

    // Types
    template<typename T>
    IntegerType_g       IntegerType();
    IntegerType_g       IntegerType(unsigned bits);
    Type_g              FloatType(unsigned bits);
    StructType_g        OpaqueType(kstring name = nullptr);
    StructType_g        StructType(StructType_g base, const Signature &body);
    StructType_g        StructType(const Signature &items, kstring n = nullptr);
    FunctionType_g      FunctionType(Type_g r,
                                     const Signature &p,bool va=false);
    PointerType_g       FunctionPointerType(Type_g r,
                                            const Signature &p,bool va=false);
    Type_g              PointerType(Type_g rty, kstring name);
    PointerType_g       MachinePointerType(Type_g wrapper) const;
    Type_g              VoidType();

    // Modules
    ModuleID            CreateModule(text name);
    void                DeleteModule(ModuleID id);

    // Functions
    Function_g          Function(FunctionType_g type, text name);
    void                Finalize(Function_g function);
    void *              ExecutableCode(Function_g f);

    // Prototypes and external functions
    Function_g          ExternFunction(FunctionType_g fty, text name);
    Function_g          Prototype(Function_g callee);
    Value_g             Prototype(Value_g callee);

};


struct JITArguments
// ----------------------------------------------------------------------------
//   Encapsulate argument lists with a nicer syntax
// ----------------------------------------------------------------------------
{
    JITArguments(JIT::Function_g function)
        : args(function->arg_begin()), count(function->arg_size()) {}

    JIT::Value_g        operator*(void)         { return &*args; }
    JITArguments &      operator++(void)        { ++args; return *this; }
    JITArguments        operator++(int)
    {
        JITArguments copy(*this);
        ++args;
        return copy;
    }
    size_t              Count()                 { return count; }

private:
    typedef llvm::Function::arg_iterator Args;
    Args                args;
    size_t              count;
};


template<typename T>
inline JIT::IntegerType_g JIT::IntegerType()
// ----------------------------------------------------------------------------
//    Return the integer type for type T
// ----------------------------------------------------------------------------
{
    return IntegerType(BitsPerByte * sizeof(T));
}


class JITModule
// ----------------------------------------------------------------------------
//    Create / delete a JIT module
// ----------------------------------------------------------------------------
{
public:
    JITModule(JIT &jit, text name)
        : jit(jit), module(jit.CreateModule(name)) {}
    JITModule(Compiler &compiler, text name);
    ~JITModule()        { jit.DeleteModule(module); }
private:
    JIT &               jit;
    JIT::ModuleID       module;
};


class JITBlock
// ----------------------------------------------------------------------------
//   Interface to the LLVM IR builder
// ----------------------------------------------------------------------------
{
    class JITPrivate      &p;
    class JITBlockPrivate &b;

public:
    JITBlock(JIT &jit, JIT::Function_g function, kstring name);
    JITBlock(const JITBlock &from, kstring name);
    JITBlock(JIT &jit);
    ~JITBlock();

    JITBlock &          operator=(const JITBlock &o);

    static JIT::Type_g  Type(JIT::Value_g value)
    {
        return JIT::Type(value);
    }
    static JIT::Type_g  ReturnType(JIT::Function_g f)
    {
        return JIT::ReturnType(f);
    }

    JIT::Constant_g     BooleanConstant(bool value);
    JIT::Constant_g     IntegerConstant(JIT::Type_g ty, uint64_t value);
    JIT::Constant_g     IntegerConstant(JIT::Type_g ty, int64_t value);
    JIT::Constant_g     IntegerConstant(JIT::Type_g ty, unsigned value);
    JIT::Constant_g     IntegerConstant(JIT::Type_g ty, int value);
    JIT::Constant_g     FloatConstant(JIT::Type_g ty, double value);
    JIT::Constant_g     NullConstant(JIT::Type_g ty);
    JIT::Constant_g     PointerConstant(JIT::Type_g pty, void *address);
    JIT::Value_g        TextConstant(JIT::Type_g pty, text value);

    void                SwitchTo(JITBlock &block);
    void                SwitchTo(JIT::BasicBlock_g block);

    JIT::Value_g        Call(JIT::Value_g c,
                             JIT::Value_g a);
    JIT::Value_g        Call(JIT::Value_g c,
                             JIT::Value_g a1, JIT::Value_g a2);
    JIT::Value_g        Call(JIT::Value_g c,
                             JIT::Value_g a1, JIT::Value_g a2, JIT::Value_g a3);
    JIT::Value_g        Call(JIT::Value_g c,
                             JIT::Values &args);

    JIT::BasicBlock_g   Block();
    JIT::BasicBlock_g   NewBlock(kstring name);
    JIT::Value_g        Return(JIT::Value_g value = nullptr);
    JIT::Value_g        Branch(JITBlock &to);
    JIT::Value_g        Branch(JIT::BasicBlock_g to);
    JIT::Value_g        IfBranch(JIT::Value_g cond,
                                 JITBlock &t, JITBlock &f);
    JIT::Value_g        IfBranch(JIT::Value_g cond,
                                 JIT::BasicBlock_g t, JIT::BasicBlock_g f);
    JIT::Value_g        Select(JIT::Value_g cond,
                               JIT::Value_g t, JIT::Value_g f);
    JIT::Value_g        IsNullPointer(JIT::Value_g pointer, kstring name = "");
    JIT::Value_g        IsOKPointer(JIT::Value_g pointer, kstring name = "");

    JIT::Value_g        Alloca(JIT::Type_g type, kstring name = "");
    JIT::Value_g        AllocateReturnValue(JIT::Function_g f,
                                            kstring name = "");
    JIT::Value_g        StructGEP(JIT::Type_g structTy,
                                  JIT::Value_g ptr,
                                  unsigned idx,
                                  kstring name="");
    JIT::Value_g        ArrayGEP(JIT::Type_g elementTy,
                                  JIT::Value_g ptr,
                                 uint32_t idx,
                                 kstring name="");
    JIT::Value_g        Load(JIT::Type_g ty,
                             JIT::Value_g ptr,
                             kstring name = "");
    JIT::Value_g        StructLoad(JIT::Type_g structTy,
                                   JIT::Value_g ptr,
                                   unsigned idx,
                                   kstring name="");
    JIT::Value_g        PointerValue(JIT::Value_g ptr);
    JIT::Value_g        WrappedValue(JIT::Value_g ptr, JIT::Type_g type);
    JIT::Value_g        BitCast(JIT::Value_g v,
                                JIT::Type_g t,
                                kstring name = "");

#define UNARY(Name)                                                     \
    JIT::Value_g        Name(JIT::Value_g l,                            \
                             kstring name = "");
#define BINARY(Name)                                                    \
    JIT::Value_g        Name(JIT::Value_g l,                            \
                             JIT::Value_g r,                            \
                             kstring name = "");
#define CAST(Name)                                                      \
    JIT::Value_g        Name(JIT::Value_g l,                            \
                             JIT::Type_g r,                             \
                             kstring name = "");
#include "llvm-crap.tbl"
};

} // namespace XL

#endif // INTERPRETER_ONLY

extern XL::JIT::Value_g xldebug(XL::JIT::Value_g);
extern XL::JIT::Type_g  xldebug(XL::JIT::Type_g);

#endif // LLVM_CRAP_H



// ============================================================================
//
//   Diagnostic interactively adjusting progressive emergency response (DIAPER)
//
// ============================================================================
//
//   This wonderful code is designed to contain the diagnostic-related damage
//   introduced, with ever increasing creativity, by the various iterations
//   of the LLVM headers, e.g. LLVM clang warnings against LLVM headers,
//   abuse and misuse of common macro names, and so on.


// ----------------------------------------------------------------------------
#ifdef LLVM_CRAP_DIAPER_OPEN
// ----------------------------------------------------------------------------
# pragma GCC diagnostic push

// It's getting harder and harder to ignore a warning
# pragma GCC diagnostic ignored "-Wpragmas"
# pragma GCC diagnostic ignored "-Wunknown-pragmas"
# pragma GCC diagnostic ignored "-Wunknown-warning-option"

// Ignore badly indented 'if' in 3.52
# if LLVM_VERSION >= 350 && LLVM_VERSION < 360
#  pragma GCC diagnostic ignored "-Wmisleading-indentation"
# endif // LLVM_VERSION >= 350 && LLVM_VERSION < 360

// All over the place
// # pragma GCC diagnostic ignored "-Wunused-parameter"

// Binding dereferenced null pointer in 3.7.1 LinkAllPasses.h
# if LLVM_VERSION >= 370 && LLVM_VERSION < 380
#  pragma GCC diagnostic ignored "-Wnull-dereference"
# endif // LLVM_VERSION >= 370 && LLVM_VERSION < 380

// memcpy in SmallVector for std::pair with non-trivial copy ctor (ongoing)
# if LLVM_VERSION >= 400
#  pragma GCC diagnostic ignored "-Wclass-memaccess"
# endif // LLVM_VERSION >= 400

// Some recent drops of LLVM have the EXTRAORINARY idea of defining DEBUG(x)
# ifdef DEBUG
#  define LLVM_CRAP_DIAPER_DEBUG       DEBUG
#  undef DEBUG
# endif // DEBUG

# undef LLVM_CRAP_DIAPER_OPEN
#endif // LLVM_CRAP_DIAPER_OPEN


// ----------------------------------------------------------------------------
#ifdef LLVM_CRAP_DIAPER_CLOSE
// ----------------------------------------------------------------------------
# pragma GCC diagnostic pop
# undef DEBUG
# ifdef LLVM_CRAP_DIAPER_DEBUG
#  define DEBUG LLVM_CRAP_DIAPER_DEBUG
# endif // LLVM_CRAP_DIAPER_DEBUG
# undef LLVM_CRAP_DIAPER_CLOSE
#endif // LLVM_CRAP_DIAPER_CLOSE
