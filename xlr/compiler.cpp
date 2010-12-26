// ****************************************************************************
//  compiler.cpp                                                    XLR project
// ****************************************************************************
// 
//   File Description:
// 
//    Just-in-time (JIT) compilation of XL trees
// 
// 
// 
// 
// 
// 
// 
// 
// ****************************************************************************
// This document is released under the GNU General Public License.
// See http://www.gnu.org/copyleft/gpl.html and Matthew 25:22 for details
//  (C) 1992-2010 Christophe de Dinechin <christophe@taodyne.com>
//  (C) 2010 Taodyne SAS
// ****************************************************************************

#include "compiler.h"
#include "compiler-gc.h"
#include "compiler-unit.h"
#include "options.h"
#include "context.h"
#include "renderer.h"
#include "runtime.h"
#include "errors.h"

#include <iostream>
#include <sstream>
#include <cstdarg>

#include <llvm/Analysis/Verifier.h>
#include <llvm/CallingConv.h>
#include "llvm/Constants.h"
#include "llvm/LLVMContext.h"
#include "llvm/DerivedTypes.h"
#include "llvm/ExecutionEngine/JIT.h"
#include "llvm/ExecutionEngine/GenericValue.h"
#include <llvm/ExecutionEngine/ExecutionEngine.h>
#include "llvm/Instructions.h"
#include "llvm/Module.h"
#include <llvm/PassManager.h>
#include "llvm/Support/raw_ostream.h"
#include <llvm/Support/IRBuilder.h>
#include <llvm/Support/StandardPasses.h>
#include <llvm/System/DynamicLibrary.h>
#include <llvm/Target/TargetData.h>
#include <llvm/Target/TargetSelect.h>
#include <llvm/Target/TargetOptions.h>
#include <llvm/Transforms/Scalar.h>
#include <llvm/Transforms/Utils/BasicBlockUtils.h>
#include <llvm/Support/raw_ostream.h>

XL_BEGIN


// ============================================================================
// 
//    Compiler - Global information about the LLVM compiler
// 
// ============================================================================
//
// The Compiler class is where we store all the global information that
// persists during the lifetime of the program: LLVM data structures,
// LLVM definitions for frequently used types, XL runtime functions, ...
// 

using namespace llvm;

static void* unresolved_external(const std::string& name)
// ----------------------------------------------------------------------------
//   Resolve external names that dyld doesn't know about
// ----------------------------------------------------------------------------
// This is really just to print a fancy error message
{
    std::cout.flush();
    std::cerr << "Unable to resolve external: " << name << std::endl;
    assert(0);
    return 0;
}


Compiler::Compiler(kstring moduleName, uint optimize_level)
// ----------------------------------------------------------------------------
//   Initialize the various instances we may need
// ----------------------------------------------------------------------------
    : module(NULL), runtime(NULL), optimizer(NULL),
      treeTy(NULL), treePtrTy(NULL), treePtrPtrTy(NULL),
      integerTreeTy(NULL), integerTreePtrTy(NULL),
      realTreeTy(NULL), realTreePtrTy(NULL),
      prefixTreeTy(NULL), prefixTreePtrTy(NULL),
      nativeTy(NULL), nativeFnTy(NULL),
      evalTy(NULL), evalFnTy(NULL),
      infoPtrTy(NULL), contextPtrTy(NULL), charPtrTy(NULL),
      xl_evaluate(NULL), xl_same_text(NULL), xl_same_shape(NULL),
      xl_infix_match_check(NULL), xl_type_check(NULL), xl_form_error(NULL),
      xl_new_integer(NULL), xl_new_real(NULL), xl_new_character(NULL),
      xl_new_text(NULL), xl_new_xtext(NULL), xl_new_block(NULL),
      xl_new_prefix(NULL), xl_new_postfix(NULL), xl_new_infix(NULL),
      xl_new_closure(NULL)
{
    // Register a listener with the garbage collector
    CompilerGarbageCollectionListener *cgcl =
        new CompilerGarbageCollectionListener(this);
    Allocator<Tree>     ::Singleton()->AddListener(cgcl);
    Allocator<Integer>  ::Singleton()->AddListener(cgcl);
    Allocator<Real>     ::Singleton()->AddListener(cgcl);
    Allocator<Text>     ::Singleton()->AddListener(cgcl);
    Allocator<Name>     ::Singleton()->AddListener(cgcl);
    Allocator<Infix>    ::Singleton()->AddListener(cgcl);
    Allocator<Prefix>   ::Singleton()->AddListener(cgcl);
    Allocator<Postfix>  ::Singleton()->AddListener(cgcl);
    Allocator<Block>    ::Singleton()->AddListener(cgcl);

    // Initialize native target (new features)
    InitializeNativeTarget();

    // LLVM Context (new feature)
    context = new llvm::LLVMContext();

    // Create module where we will build the code
    module = new Module(moduleName, *context);

    // Select "fast JIT" if optimize level is 0, optimizing JIT otherwise
    runtime = EngineBuilder(module).create();
    runtime->DisableLazyCompilation(false);

    // Setup the optimizer - REVISIT: Adjust with optimization level
    optimizer = new FunctionPassManager(module);
    createStandardFunctionPasses(optimizer, optimize_level);
    {
        // Register target data structure layout info
        optimizer->add(new TargetData(*runtime->getTargetData()));

        // Promote allocas to registers.
        optimizer->add(createPromoteMemoryToRegisterPass());

        // Do simple "peephole" optimizations and bit-twiddling optimizations.
        optimizer->add(createInstructionCombiningPass());

        // Inlining of tails
        optimizer->add(createTailDuplicationPass());
        optimizer->add(createTailCallEliminationPass());

        // Re-order blocks to eliminate branches
        optimizer->add(createBlockPlacementPass());

        // Collapse duplicate variables into canonical form
        // optimizer->add(createPredicateSimplifierPass());

        // Reassociate expression for better constant propagation
        optimizer->add(createReassociatePass());

        // Eliminate common subexpressions.
        optimizer->add(createGVNPass());

        // Simplify the control flow graph (deleting unreachable blocks, etc).
        optimizer->add(createCFGSimplificationPass());

        // Place phi nodes at loop boundaries to simplify other loop passes
        optimizer->add(createLCSSAPass());

        // Loop invariant code motion and memory promotion
        optimizer->add(createLICMPass());

        // Transform a[n] into *ptr++
        optimizer->add(createLoopStrengthReducePass());

        // Unroll loops (can it help in our case?)
        optimizer->add(createLoopUnrollPass());
    }

    // Other target options
    // DwarfExceptionHandling = true;// Present in LLVM 2.6, but crashes
    JITEmitDebugInfo = true;         // Not present in LLVM 2.6
    UnwindTablesMandatory = true;
    // PerformTailCallOpt = true;
    NoFramePointerElim = true;

    // Install a fallback mechanism to resolve references to the runtime, on
    // systems which do not allow the program to dlopen itself.
    runtime->InstallLazyFunctionCreator(unresolved_external);

    // Create the Info and Symbol pointer types
    PATypeHolder structInfoTy = OpaqueType::get(*context); // struct Info
    infoPtrTy = PointerType::get(structInfoTy, 0);         // Info *
    PATypeHolder structCtxTy = OpaqueType::get(*context);  // struct Context
    contextPtrTy = PointerType::get(structCtxTy, 0);       // Context *

    // Create the Tree and Tree pointer types
    PATypeHolder structTreeTy = OpaqueType::get(*context); // struct Tree
    treePtrTy = PointerType::get(structTreeTy, 0);      // Tree *
    treePtrPtrTy = PointerType::get(treePtrTy, 0);      // Tree **

    // Create the native_fn type
    std::vector<const Type *> nativeParms;
    nativeParms.push_back(contextPtrTy);
    nativeParms.push_back(treePtrTy);
    nativeTy = FunctionType::get(treePtrTy, nativeParms, false);
    nativeFnTy = PointerType::get(nativeTy, 0);

    // Create the eval_fn type
    std::vector<const Type *> evalParms;
    evalParms.push_back(treePtrTy);
    evalTy = FunctionType::get(treePtrTy, evalParms, false);
    evalFnTy = PointerType::get(evalTy, 0);

    // Verify that there wasn't a change in the Tree type invalidating us
    struct LocalTree
    {
        LocalTree (const Tree &o): tag(o.tag), info(o.info) {}
        ulong    tag;
        XL::Info*info;          // We check that the size is the same
    };
    // If this assert fails, you changed struct tree and need to modify here
    XL_CASSERT(sizeof(LocalTree) == sizeof(Tree));
               
    // Create the Tree type
    std::vector<const Type *> treeElements;
    treeElements.push_back(LLVM_INTTYPE(ulong));           // tag
    treeElements.push_back(infoPtrTy);                     // info
    treeTy = StructType::get(*context, treeElements);      // struct Tree {}
    cast<OpaqueType>(structTreeTy.get())->refineAbstractTypeTo(treeTy);
    treeTy = cast<StructType> (structTreeTy.get());

    // Create the Integer type
    std::vector<const Type *> integerElements = treeElements;
    integerElements.push_back(LLVM_INTTYPE(longlong));  // value
    integerTreeTy = StructType::get(*context, integerElements); // struct Int
    integerTreePtrTy = PointerType::get(integerTreeTy,0); // Integer *

    // Create the Real type
    std::vector<const Type *> realElements = treeElements;
    realElements.push_back(Type::getDoubleTy(*context));  // value
    realTreeTy = StructType::get(*context, realElements); // struct Real{}
    realTreePtrTy = PointerType::get(realTreeTy, 0);      // Real *

    // Create the Prefix type (which we also use for Infix and Block)
    std::vector<const Type *> prefixElements = treeElements;
    prefixElements.push_back(treePtrTy);                // Tree *
    prefixElements.push_back(treePtrTy);                // Tree *
    prefixTreeTy = StructType::get(*context, prefixElements); // struct Prefix
    prefixTreePtrTy = PointerType::get(prefixTreeTy, 0);// Prefix *

    // Record the type names
    module->addTypeName("tree", treeTy);
    module->addTypeName("integer", integerTreeTy);
    module->addTypeName("real", realTreeTy);
    module->addTypeName("eval", evalTy);
    module->addTypeName("prefix", prefixTreeTy);
    module->addTypeName("info*", infoPtrTy);

    // Create a reference to the evaluation function
    charPtrTy = PointerType::get(LLVM_INTTYPE(char), 0);
    const Type *boolTy = Type::getInt1Ty(*context);
#define FN(x) #x, (void *) XL::x
    xl_evaluate = ExternFunction(FN(xl_evaluate),
                                 treePtrTy, 2, contextPtrTy, treePtrTy);
    xl_same_text = ExternFunction(FN(xl_same_text),
                                  boolTy, 2, treePtrTy, charPtrTy);
    xl_same_shape = ExternFunction(FN(xl_same_shape),
                                   boolTy, 2, treePtrTy, treePtrTy);
    xl_infix_match_check = ExternFunction(FN(xl_infix_match_check),
                                          treePtrTy, 2, treePtrTy, charPtrTy);
    xl_type_check = ExternFunction(FN(xl_type_check), treePtrTy,
                                   3, contextPtrTy, treePtrTy, treePtrTy);
    xl_form_error = ExternFunction(FN(xl_form_error),
                                   treePtrTy, 1, treePtrTy);
    xl_new_integer = ExternFunction(FN(xl_new_integer),
                                    treePtrTy, 1, LLVM_INTTYPE(longlong));
    xl_new_real = ExternFunction(FN(xl_new_real),
                                 treePtrTy, 1, Type::getDoubleTy(*context));
    xl_new_character = ExternFunction(FN(xl_new_character),
                                      treePtrTy, 1, charPtrTy);
    xl_new_text = ExternFunction(FN(xl_new_text),
                                 treePtrTy, 1, charPtrTy);
    xl_new_xtext = ExternFunction(FN(xl_new_xtext),
                                 treePtrTy, 3, charPtrTy, charPtrTy, charPtrTy);
    xl_new_block = ExternFunction(FN(xl_new_block),
                                  treePtrTy, 2, treePtrTy,treePtrTy);
    xl_new_prefix = ExternFunction(FN(xl_new_prefix),
                                   treePtrTy, 3, treePtrTy,treePtrTy,treePtrTy);
    xl_new_postfix = ExternFunction(FN(xl_new_postfix),
                                    treePtrTy, 3,treePtrTy,treePtrTy,treePtrTy);
    xl_new_infix = ExternFunction(FN(xl_new_infix),
                                  treePtrTy, 3, treePtrTy,treePtrTy,treePtrTy);
    xl_new_closure = ExternFunction(FN(xl_new_closure),
                                    treePtrTy, -2,
                                    treePtrTy, LLVM_INTTYPE(uint));
}


Compiler::~Compiler()
// ----------------------------------------------------------------------------
//    Destructor deletes the various things we had created
// ----------------------------------------------------------------------------
{
    delete context;
}


void Compiler::Reset()
// ----------------------------------------------------------------------------
//    Clear the contents of a compiler
// ----------------------------------------------------------------------------
{
    closures.clear();
}


CompilerInfo *Compiler::Info(Tree *tree, bool create)
// ----------------------------------------------------------------------------
//   Find or create the compiler-related info for a given tree
// ----------------------------------------------------------------------------
{
    CompilerInfo *result = tree->GetInfo<CompilerInfo>();
    if (!result && create)
    {
        result = new CompilerInfo(tree);
        tree->SetInfo<CompilerInfo>(result);
    }
    return result;
}


llvm::Function * Compiler::TreeFunction(Tree *tree)
// ----------------------------------------------------------------------------
//   Return the function associated to the tree
// ----------------------------------------------------------------------------
{
    CompilerInfo *info = Info(tree);
    return info ? info->function : NULL;
}


void Compiler::SetTreeFunction(Tree *tree, llvm::Function *function)
// ----------------------------------------------------------------------------
//   Associate a function to the given tree
// ----------------------------------------------------------------------------
{
    CompilerInfo *info = Info(tree, true);
    info->function = function;
}


llvm::GlobalValue * Compiler::TreeGlobal(Tree *tree)
// ----------------------------------------------------------------------------
//   Return the global value associated to the tree, if any
// ----------------------------------------------------------------------------
{
    CompilerInfo *info = Info(tree);
    return info ? info->global : NULL;
}


void Compiler::SetTreeGlobal(Tree *tree, llvm::GlobalValue *global, void *addr)
// ----------------------------------------------------------------------------
//   Set the global value associated to the tree
// ----------------------------------------------------------------------------
{
    CompilerInfo *info = Info(tree, true);
    info->global = global;
    runtime->addGlobalMapping(global, addr ? addr : &info->tree);
}


Function *Compiler::EnterBuiltin(text name,
                                 Tree *to,
                                 TreeList parms,
                                 eval_fn code)
// ----------------------------------------------------------------------------
//   Declare a built-in function
// ----------------------------------------------------------------------------
//   The input is not technically an eval_fn, but has as many parameters as
//   there are variables in the form
{
    IFTRACE(llvm)
        std::cerr << "EnterBuiltin " << name
                  << " C" << (void *) code << " T" << (void *) to;

    Function *result = builtins[name];
    if (result)
    {
        IFTRACE(llvm)
            std::cerr << " existing F " << result
                      << " replaces F" << TreeFunction(to) << "\n";
        SetTreeFunction(to, result);
    }
    else
    {
        // Create the LLVM function
        std::vector<const Type *> parmTypes;
        parmTypes.push_back(treePtrTy); // First arg is self
        for (TreeList::iterator p = parms.begin(); p != parms.end(); p++)
            parmTypes.push_back(treePtrTy);
        FunctionType *fnTy = FunctionType::get(treePtrTy, parmTypes, false);
        result = Function::Create(fnTy, Function::ExternalLinkage,
                                  name, module);

        // Record the runtime symbol address
        sys::DynamicLibrary::AddSymbol(name, (void*) code);

        IFTRACE(llvm)
            std::cerr << " new F " << result
                      << "replaces F" << TreeFunction(to) << "\n";

        // Associate the function with the tree form
        SetTreeFunction(to, result);
        builtins[name] = result;
    }

    return result;    
}


adapter_fn Compiler::ArrayToArgsAdapter(uint numargs)
// ----------------------------------------------------------------------------
//   Generate code to call a function with N arguments
// ----------------------------------------------------------------------------
//   The generated code serves as an adapter between code that has
//   tree arguments in a C array and code that expects them as an arg-list.
//   For example, it allows you to call foo(Tree *src, Tree *a1, Tree *a2)
//   by calling generated_adapter(foo, Tree *src, Tree *args[2])
{
    IFTRACE(llvm)
        std::cerr << "EnterArrayToArgsAdapater " << numargs;

    // Check if we already computed it
    adapter_fn result = array_to_args_adapters[numargs];
    if (result)
    {
        IFTRACE(llvm)
            std::cerr << " existing C" << (void *) result << "\n";
        return result;
    }

    // Generate the function type:
    // Tree *generated(Context *, native_fn, Tree *, Tree **)
    std::vector<const Type *> parms;
    parms.push_back(nativeFnTy);
    parms.push_back(contextPtrTy);
    parms.push_back(treePtrTy);
    parms.push_back(treePtrPtrTy);
    FunctionType *fnType = FunctionType::get(treePtrTy, parms, false);
    Function *adapter = Function::Create(fnType, Function::InternalLinkage,
                                        "xl_adapter", module);

    // Generate the function type for the called function
    std::vector<const Type *> called;
    called.push_back(contextPtrTy);
    called.push_back(treePtrTy);
    for (uint a = 0; a < numargs; a++)
        called.push_back(treePtrTy);
    FunctionType *calledType = FunctionType::get(treePtrTy, called, false);
    PointerType *calledPtrType = PointerType::get(calledType, 0);

    // Create the entry for the function we generate
    BasicBlock *entry = BasicBlock::Create(*context, "adapt", adapter);
    IRBuilder<> code(entry);

    // Read the arguments from the function we are generating
    Function::arg_iterator inArgs = adapter->arg_begin();
    Value *fnToCall = inArgs++;
    Value *contextPtr = inArgs++;
    Value *sourceTree = inArgs++;
    Value *treeArray = inArgs++;

    // Cast the input function pointer to right type
    Value *fnTyped = code.CreateBitCast(fnToCall, calledPtrType, "fnCast");

    // Add source as first argument to output arguments
    std::vector<Value *> outArgs;
    outArgs.push_back (contextPtr);
    outArgs.push_back (sourceTree);

    // Read other arguments from the input array
    for (uint a = 0; a < numargs; a++)
    {
        Value *elementPtr = code.CreateConstGEP1_32(treeArray, a);
        Value *fromArray = code.CreateLoad(elementPtr, "arg");
        outArgs.push_back(fromArray);
    }

    // Call the function
    Value *retVal = code.CreateCall(fnTyped, outArgs.begin(), outArgs.end());

    // Return the result
    code.CreateRet(retVal);

    // Verify the function and optimize it.
    verifyFunction (*adapter);
    if (optimizer)
        optimizer->run(*adapter);

    // Enter the result in the map
    result = (adapter_fn) runtime->getPointerToFunction(adapter);
    array_to_args_adapters[numargs] = result;

    IFTRACE(llvm)
        std::cerr << " new C" << (void *) result << "\n";

    // And return it to the caller
    return result;
}


Function *Compiler::ExternFunction(kstring name, void *address,
                                   const Type *retType, int parmCount, ...)
// ----------------------------------------------------------------------------
//   Return a Function for some given external symbol
// ----------------------------------------------------------------------------
{
    IFTRACE(llvm)
        std::cerr << "ExternFunction " << name
                  << " has " << parmCount << " parameters "
                  << " C" << address;

    va_list va;
    std::vector<const Type *> parms;
    bool isVarArg = parmCount < 0;
    if (isVarArg)
        parmCount = -parmCount;

    va_start(va, parmCount);
    for (int i = 0; i < parmCount; i++)
    {
        Type *ty = va_arg(va, Type *);
        parms.push_back(ty);
    }
    va_end(va);
    FunctionType *fnType = FunctionType::get(retType, parms, isVarArg);
    Function *result = Function::Create(fnType, Function::ExternalLinkage,
                                        name, module);
    sys::DynamicLibrary::AddSymbol(name, address);

    IFTRACE(llvm)
        std::cerr << " F" << result << "\n";

    return result;
}


Value *Compiler::EnterGlobal(Name *name, Name_p *address)
// ----------------------------------------------------------------------------
//   Enter a global variable in the symbol table
// ----------------------------------------------------------------------------
{
    Constant *null = ConstantPointerNull::get(treePtrTy);
    bool isConstant = false;
    GlobalValue *result = new GlobalVariable (*module, treePtrTy, isConstant,
                                              GlobalVariable::ExternalLinkage,
                                              null, name->value);
    SetTreeGlobal(name, result, address);

    IFTRACE(llvm)
        std::cerr << "EnterGlobal " << name->value
                  << " name T" << (void *) name
                  << " A" << address
                  << " address T" << (void *) address->Pointer()
                  << "\n";

    return result;
}


Value *Compiler::EnterConstant(Tree *constant)
// ----------------------------------------------------------------------------
//   Enter a constant (i.e. an Integer, Real or Text) into global map
// ----------------------------------------------------------------------------
{
    bool isConstant = true;
    text name = "xlcst";
    switch(constant->Kind())
    {
    case INTEGER: name = "xlint";  break;
    case REAL:    name = "xlreal"; break;
    case TEXT:    name = "xltext"; break;
    default:                       break;
    }
    IFTRACE(labels)
        name += "[" + text(*constant) + "]";
    GlobalValue *result = new GlobalVariable (*module, treePtrTy, isConstant,
                                              GlobalVariable::InternalLinkage,
                                              NULL, name);
    SetTreeGlobal(constant, result, NULL);

    IFTRACE(llvm)
        std::cerr << "EnterConstant T" << (void *) constant
                  << " A" << (void *) &Info(constant)->tree << "\n";

    return result;
}


eval_fn Compiler::MarkAsClosure(XL::Tree *closure, uint ntrees)
// ----------------------------------------------------------------------------
//    Create the closure wrapper for ntrees elements, associate to result
// ----------------------------------------------------------------------------
{
    eval_fn fn = closures[ntrees];
    if (!fn)
    {
        TreeList noParms;
        CompiledUnit unit(this, closure, noParms);
        unit.CallClosure(closure, ntrees);
        fn = unit.Finalize();
        closures[ntrees] = fn;
        SetTreeFunction(closure, NULL); // Now owned by closures[n]
    }

    return fn;
}


bool Compiler::IsKnown(Tree *tree)
// ----------------------------------------------------------------------------
//    Test if global is known
// ----------------------------------------------------------------------------
{
    return TreeGlobal(tree) != NULL;
}


bool Compiler::FreeResources(Tree *tree)
// ----------------------------------------------------------------------------
//   Free the LLVM resources associated to the tree, if any
// ----------------------------------------------------------------------------
//   In the first pass, we need to clear the body and machine code for all
//   functions. This is because if we have foo() calling bar() and bar()
//   calling foo(), we will get an LLVM assert deleting one while the
//   other's body still makes a reference.
{
    bool result = true;

    IFTRACE(llvm)
        std::cerr << "FreeResources T" << (void *) tree;

    CompilerInfo *info = Info(tree);
    if (!info)
    {
        IFTRACE(llvm)
            std::cerr << " has no info\n";
        return true;
    }

    // Drop function reference if any
    if (Function *f = info->function)
    {
        bool inUse = !f->use_empty();
        
        IFTRACE(llvm)
            std::cerr << " function F" << f
                      << (inUse ? " in use" : " unused");
        
        if (inUse)
        {
            // Defer deletion until later
            result = false;
        }
        else
        {
            // Not in use, we can delete it directly
            f->eraseFromParent();
            info->function = NULL;
        }
    }
    
    // Drop any global reference
    if (GlobalValue *v = info->global)
    {
        bool inUse = !v->use_empty();
        
        IFTRACE(llvm)
            std::cerr << " global V" << v
                      << (inUse ? " in use" : " unused");
        
        if (inUse)
        {
            // Defer deletion until later
            result = false;
        }
        else
        {
            // Delete the LLVM value immediately if it's safe to do it.
            runtime->updateGlobalMapping(v, NULL);
            v->eraseFromParent();
            info->global = NULL;
        }
    }

    IFTRACE(llvm)
        std::cerr << (result ? " Delete\n" : "Preserved\n");

    return result;
}

XL_END


// ============================================================================
// 
//    Debug helpers
// 
// ============================================================================

void debugm(XL::value_map &m)
// ----------------------------------------------------------------------------
//   Dump a value map from the debugger
// ----------------------------------------------------------------------------
{
    XL::value_map::iterator i;
    for (i = m.begin(); i != m.end(); i++)
        llvm::errs() << "map[" << (*i).first << "]=" << *(*i).second << '\n';
}


void debugv(void *v)
// ----------------------------------------------------------------------------
//   Dump a value for the debugger
// ----------------------------------------------------------------------------
{
    llvm::Value *value = (llvm::Value *) v;
    llvm::errs() << *value << "\n";
}
