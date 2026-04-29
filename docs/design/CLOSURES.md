# Boxed closures and LLVM: design notes for XL (xlr)

This document records how **closures** work in the XL **interpreter** today, walks a
small **O3** example (`tests/O3/assign.xl`), contrasts that with the **fast
compiler** path (`xl_new_closure`), and outlines **design options** for
representing **boxed** closures in LLVM IR with trade-offs.  It is meant to
guide implementation; it is not a commitment to a single strategy.

---

## 1. Interpreter representation

### 1.1 `Context::Closure`

`Context::Closure(Tree *value)` (`src/context.cpp`) builds an **annotated
prefix** that pairs the **current symbol scope** with a **delayed** subtree.

Rough algorithm:

- If `value` is a `NAME`, follow `context->Bound` and unwrap nested closures
  when the bound value is already a closure whose inner payload differs from
  the name (retry loop).
- If the value still needs environment capture (`valueKind >= NAME` or the
  context has rewrites for that kind), and the node is **not** already a closure
  prefix, it allocates:

  - `Prefix *` with `left = context->Symbols()` (innermost `Scope *` chain
    node) and `right = value` (the expression to evaluate later).
  - A `ClosureInfo` marker attached with `SetInfo` so ordinary prefixes are not
    mistaken for closures.

So the runtime shape is:

```text
Closure  ::=  Prefix( captured_scope , body )
              + Info: ClosureInfo
```

where `Scope` is `typedef Prefix` (`include/context.h`): the **scope chain**
uses the same node kind as prefix; `Enclosing(scope)` reads `scope->left` as the
parent scope link (`include/context.h`, inline `Enclosing`).

The comment block in `include/context.h` (section **CLOSURES**) states the same
idea in user-facing terms: a closure is like extra declarations in front of the
code, i.e. `{ X -> 17 } { write X+1 }` as a prefix of scope over body.

### 1.2 `Tree::IsClosure` and `Scope::IsClosure`

`Tree::IsClosure(Context_g *context)` (`include/tree.h`) forwards to
`Scope::IsClosure` only when `AsScope()` succeeds (i.e. the tree is a `Prefix`).

`Scope::IsClosure` (`src/tree.cpp`):

- Returns `nullptr` unless the prefix carries `ClosureInfo`.
- If present, optionally sets `*context = new Context(Enclosing(this))` where
  `Enclosing(this)` is `this->left->As<Scope>()` — for a closure prefix,
  `this->left` is the **captured** innermost scope node, so the new `Context`
  uses that scope chain as its symbol table root.
- Returns `right`, i.e. the **payload** tree without the scope wrapper.

Thus **unwrap** = “replace current context’s scope view with the captured chain,
then continue with the inner tree.”

### 1.3 Where closures are created and consumed

**Creation**

- `Bindings::BindClosure` (`src/interpreter.cpp`) calls `context->Closure(value)`
  before `Define`, so pattern variables bound **without** `MustEvaluate` keep
  their lexical environment (typical for rewrite parameters that are trees, not
  pre-evaluated values).
- After a successful `evalLookup`, the rewrite body is wrapped with
  `locals->Closure(result)` before return (`evalLookup` in `interpreter.cpp`),
  so the callee receives a body that carries the **declaration-local** scope.
- `encloseResult` wraps results when the active scope changed vs. the original
  evaluation scope.

**Consumption**

- `Interpreter::Instructions` unwraps when `Lookup` returns a closure: it sets
  `what` to the inner tree and **continues** the loop with the updated
  `context` from `IsClosure(&context)`.
- Prefix application peels a closure on the **callee** before dispatch.
- `Bindings::MustEvaluate(..., unwrap=true)` can strip one closure layer when
  matching requires a concrete shape after evaluation.
- `Interpreter::Evaluate` strips a final closure layer for a plain “evaluate to
  value” API.

Together, this implements **lazy trees with lexical scope**: the tree is the
primary value; the closure is a **scope-tagged tree**.

### 1.4 Lazy vs immediate binding for rewrite parameters (NORMAL)

**Specification (authoritative):** `docs/HANDBOOK.adoc` separates **immediate
evaluation** (`#immediate-evaluation`) from **lazy evaluation**
(`#lazy-evaluation`): if matching the pattern **requires** evaluating part of an
argument (or a sub-expression), that evaluation happens **for the match**; if
not, the argument remains a **tree** and is bound **lazily**.

In this repo, **`Types::Decl::NORMAL`** (`src/types.h`, used as
`CompilerTypes::Decl::NORMAL` in codegen) is the compiler-side category that
corresponds to that default: parameters are **lazy**
(like interpreter `BindClosure` / delayed O3 values) **unless** the match or RHS
**forces** evaluation.

**Typical reasons to force evaluation (not exhaustive; see the handbook and
`Bindings::MustEvaluate` in `src/interpreter.cpp`):**

- **Concrete type in the pattern**, e.g. `write X:integer`: selecting the rule
  needs the argument’s **type** (or enough work to decide the overload), so that
  parameter is not “pure tree” through the match in the same way as a bare name.
- **Concrete value in the pattern**, e.g. `write [[true]]`, other metabox forms,
  or comparisons to literals (e.g. naturals in `N!` / guards) where the match
  **depends** on a known value.

**If the pattern does not impose such requirements**, **NORMAL = lazy for every
parameter**, independent of surface syntax (`write Head, Rest` is one example
of pattern matching; the same rule applies elsewhere). In particular,
`while Condition loop Body` (`src/builtins.xl`) does **not** need to evaluate
`Condition` or `Body` **just to match** the call; both should be **lazy-bound**
like the interpreter, with evaluation only when the expanded rewrite **uses**
them (e.g. when `if Condition then …` needs a boolean). O3 thick closures and
`DoRewrite` should align with that over time; see `AGENTS.md`.

**O3 implementation (in progress):** `CompilerExpression::DoRewrite` passes thick
closures for names and for any composite argument tree (`ArgWantsLazyThickClosure`:
`NAME` or `!IsLeaf()`). No surface-shape heuristics—only this structural rule.
`DoCall` peels operand positions that are still bare `Name` nodes when the lazy
map / `Bound` supplies a **non-leaf** expansion—implemented uniformly for
prefix, postfix, and infix call shapes (XL’s representation of operators), not a
prefix-only fast path. Operator names are never taken from `lazyBindingSource`.
`Do(Block)` remains “compile the child”. Peel uses non-leaf `exp` (`!IsLeaf()`).
Populating `TreeRewriteCalls` for a
synthetic peeled tree often wants `types->Type(peeled)`, which has tripped
shared-tree hazards and O3 runtime faults in text/character write paths
(`print "a", ' ', "b"`); resolving that is separate from the peel condition.

---

## 2. Example: `tests/O3/assign.xl` and `print "ival=", ival`

Source (abbreviated):

```xl
ival := 1
print "ival=", ival
```

Built-in definition (`src/builtins.xl`):

```xl
print Items             is { write Items; print }
```

For the call `print "ival=", ival`:

1. Matching `print Items` binds `Items` to the actual argument subtree
   (`"ival=", ival` as parsed / grouped by syntax).
2. That binding uses **`BindClosure`**, so `Items` is stored as
   `Closure(Prefix(captured_scope, comma_tree))`, not as a pre-evaluated list of
   printed values.
3. The body `{ write Items; print }` is itself wrapped (via `locals->Closure` in
   `evalLookup`) so that when it runs later, names and rewrites resolve in the
   callee’s **declaration** context.
4. When `write Items` runs, evaluation of `Items` unwraps the closure: names
   like `ival` resolve in the **caller’s** captured scope, so `ival` is `1` on
   the first line.

So the interesting property is **not** “LLVM knows about `ival`” but “the
**tree** for `Items` still contains the name `ival`, and the closure prefix
carries the **Scope** chain where `ival` is bound.”  O3 must preserve that
semantic when values become **boxed machine representations** instead of raw
`Tree *`.

### 2.1 End-to-end trace: `print "ival=", ival` (interpreter / fast / O3)

Follow the same call as above, but step-by-step through the **recursive
rewrites** (`print Items`, then `write Head, Rest`, then leaves).  For each
step: **where the environment and delayed trees live** in the interpreter, in
the fast compiler, and in **O3**.  **LLVM type names** refer to `Compiler`’s
cached handles in `src/compiler.cpp` (`treeTy`, `scopeTy`, `evalTy`, …).

**Surface rewrite rules** (`src/builtins.xl`):

```xl
print Items             is { write Items; print }
write Head, Rest        is { write Head; write Rest }
write X:text            as boolean      is C xl_write_text
write X:natural         as boolean      is C xl_write_natural
…
print                   as boolean      is C xl_write_cr
```

“Comma tree” below means the parse tree for the **`,`** grouping of
`"ival=", ival` (an `Infix` with operator `,` in the usual XL encoding).

#### Step A — Match `print "ival=", ival` as `print Items`

| Engine | Where `Items`, scope, and body live |
|--------|----------------------------------------|
| **Interpreter** | **Lookup** finds `print Items`.  Pattern bind **`Items`** via `BindClosure` (`src/interpreter.cpp`): symbol table stores **`Prefix(caller_scope, comma_tree)`** + `ClosureInfo`—GC **`Tree *`**.  Returned body `{ write Items; print }` is wrapped with **`locals->Closure`** in **`evalLookup`**, so the **block** keeps the **callee** rewrite environment while **`Items`** still names the **caller-bound** delayed subtree. |
| **Fast (O1)** | If `Items` is a non-trivial lazy value, **`ArgumentMatch::CompileClosure`** may emit **`xl_new_closure`**: GC **`Block`** of synthetic binds + expr, plus attached **`eval_fn`** for an **`O1CompileUnit`** over that source (`src/compiler-fast.cpp`).  **`eval_fn`** follows **`evalTy`**: **`treePtrTy (scopePtrTy, treePtrTy)`**—first argument is **`Scope *`**.  **`ClosureAdapter`** passes **`scopePtr`** and loads captured arguments from the GC closure shape. |
| **O3** | Inference + **`CompilerExpression::Do(Prefix *)`** drive codegen; **`AddBoxedType`** links XL types to LLVM (`src/compiler-expr.cpp`).  **Leaf** `write` matches lower to **scalars / small structs** and **C `extern`** (e.g. **`xl_write_text`**), not to returning **`Tree *`**.  **Target for delayed `Items`** (design in §5.2; **invoke still a stub** in `CompilerExpression::Evaluate` when `IsClosureType`): a **thick pointer** **`{ data*, invoke* }`** with **`data*`** = **boxed scope** (machine analogue of the **`Scope *`** paired with **`eval_fn`** today). |

**LLVM layouts** (`src/compiler.cpp`):

- **`treeTy`**: `{ i64 tag, Info* }` (header shared by all boxed tree shapes).
- **`treePtrTy`**: `Tree*` in IR (`%Tree_g*`).
- **`scopeTy`**: same struct layout as a **`Prefix`** node: tree header plus
  **`treePtrTy` left** (parent scope) and **`treePtrTy` right** (rewrite list);
  see **`Enclosing`** / **`ScopeRewrites`** in `include/context.h`.
- **`evalTy`**: LLVM function type **`treePtrTy(scopePtrTy, treePtrTy)`** ≡ `Tree *(*)(Scope *, Tree *)`.
- **`evalFnTy`**: **`evalTy*`** (pointer to that function type).

**Illustrative thick-pointer IR** (design target from §5.2; not necessarily one
named struct today): e.g. **`{ ptr env, ptr fn }`** where **`fn`** is typed as
**`i8* (i8*, …)`** (opaque **`data*`** first, then any **fixed** thunk
parameters).  **`env`** would lower to the **boxed scope** struct for that
site—field types are the same **`naturalTy`**, **`textTy`**, … the O3 function
would otherwise load from **`Scope *`** after a rewrite match.

#### Step B — Evaluate `{ write Items; print }`

| Engine | Data |
|--------|------|
| **Interpreter** | **Block**: inner scope for local declarations; **`write Items`** runs with **`Context`** where **`Items`** resolves to the **closure value** from step A; **`IsClosure`** restores **`Context(captured_scope)`** before the comma tree is used. |
| **Fast** | Nested **`O1CompileUnit`** / emitted calls; every **`eval_fn`** receives the **same lexical `scopePtr`** convention plus **`Tree *`** operands (`treePtrTy`). |
| **O3** | Sequential IR for **`write`** then **`print`**.  **`CompilerFunction::InitializeArgs`** seeds **`values[scope_type]`** / **`xl_self`** (`src/compiler-function.cpp`)—often **`PointerConstant(compiler.scopePtrTy, scope)`** for **static** scope on optimized paths, or **SSA parameters** for inner generated functions. |

#### Step C — `write Items` matches `write Head, Rest`

| Engine | Data |
|--------|------|
| **Interpreter** | Unwrap **`Items`** → inner **comma tree**.  **`BindClosure(Head, …)`** binds the **left** operand (the **`text`** literal **`"ival="`**) and **`BindClosure(Rest, …)`** binds **`ival`** (**`Name *`** tree + **caller `Scope *`**) so each arm keeps the right environment. |
| **Fast** | Per-arm compilation with **`EnvironmentScan`** over **`Enclosing(symbols)`**; closure captures materialized as **extra `treePtrTy` parameters** or GC block fields for **`xl_new_closure`**. |
| **O3** | Same **shape split** as source: when **`TreeRewriteCalls`** is absent, **`Bound`** on argument names can supply **shaped** trees for **`write X:text`** etc. (`compiler-expr.cpp`).  **Literal `Head`** tends toward **`textTreePtrTy` / `textTy`**; **`Rest` (`ival`)** toward **`nameTreePtrTy`** or (when lowered) **loaded `naturalTy`** from the **boxed environment**. |

#### Step D — `write "ival="` → `write X:text` → `xl_write_text`

| Engine | Data |
|--------|------|
| **Interpreter** | Pattern + type path; opcode calls **`xl_write_text`** (`src/runtime.cpp`). |
| **Fast** | **`xl_write_text`** declared for JIT (`compiler-primitives.tbl`); argument from **`textTreePtrTy`** / `textTy` loads if still tree-shaped. |
| **O3** | **`Primitive`**: **LLVM `call`** to **`@xl_write_text`** with **`i8*`** / **`char*`** from the **`textTy`** payload (`{ char* }`); result **`booleanTy`** (`i1`). |

#### Step E — `write ival` → `write X:natural`

| Engine | Data |
|--------|------|
| **Interpreter** | **`ival`** is looked up in the **`Scope *`** carried by **`Rest`’s** closure; value **`1`** (`Natural`); opcode path calls **`xl_write_natural`** (`src/runtime.cpp`). |
| **Fast** | Operand may still be **`naturalTreePtrTy`** / loads from **`Tree *`**; **`xl_write_natural`** in JIT (`compiler-primitives.tbl`). |
| **O3** | **`Primitive`**: **`call`** to **`@xl_write_natural`** with **`i64`** (or the configured **`naturalTy`** width) after **`Autobox`** / **`ValueMachineType`**; result **`booleanTy`**. |

#### Step F — second `print` (newline)

| Engine | Data |
|--------|------|
| **Interpreter** | **`xl_write_cr`** opcode, no argument tree. |
| **Fast** | Same **`extern`**. |
| **O3** | **`Primitive`** lowers to a **`call`** to the **`xl_write_cr`** symbol with the arity/types the JIT table / runtime declare (typically **boolean** result, no tree argument). |

#### Takeaway

Language recursion is **`print` → block → `write Items` → `write Head, Rest` →
two leaf `write`s → `print`**.  Implementation recursion lines up as: **GC
`Tree *` + `Scope *` + closure prefixes** (interpreter); **`eval_fn` with
`scopePtrTy` first + optional `xl_new_closure` GC block** (fast); **typed IR +
`call @xl_write_*` for leaves**, plus the **target** thick **`{ data*, invoke*
}`** for delayed parameters like **`Items`** (O3).

---

## 3. Current fast-compiler path (non-boxed closure value)

The O1 / “fast” path already implements a **different** closure representation
for **compiled delayed evaluation** (`src/compiler-fast.cpp`):

- `ArgumentMatch::CompileClosure` scans free names (`EnvironmentScan`), pairs
  names with LLVM-known values, and may emit **`xl_new_closure`**.
- `xl_new_closure` (`compiler-fast.cpp`) builds a **`Block`** holding a chain of
  `P is V` declarations (as `Infix`) ending with the expression, attaches an
  **`eval_fn`** via `FastCompiler::SetTreeCode`, and returns that **GC tree**.
- `ClosureAdapter(n)` generates a small LLVM thunk that loads captured `Tree *`
  values out of the block shape and calls the specialized `eval_fn`.

`AGENTS.md` already warns: **`xl_new_closure` output is not LLVM-boxed**; O3
codegen expects **boxed** machine types and `CompilerFunction::Autobox`.  The
open problem is bridging **“closure as GC tree + C eval_fn”** to **“closure as
first-class boxed value in the LLVM type system.”**

`CompilerExpression::Evaluate` (`src/compiler-expr.cpp`) reserves a spot for
`IsClosureType` + “invoke closure” but that path is still a stub — another sign
the boxed story is incomplete.

---

## 4. What “boxed closure in LLVM” needs to mean for XL

At minimum, a boxed closure value must support:

1. **Construction** at a call site with captured bindings (trees or typed
   values, depending on rewrite / `MustEvaluate` rules).
2. **Invocation** at a use site: restore lexical access semantics consistent with
   `IsClosure` + `Instructions`.
3. **Interop** with the rest of O3: **uniform calling conventions**, GC /
   lifetime rules, and `ValueMachineType` / `Autobox` coherence (see
   `AGENTS.md`: nested closures and mixed boxed vs raw environment models).

The interpreter stores **entire scope chains** in the prefix `left`.  A
compiler may instead store **only the free variables** actually referenced
(like `CompileClosure` today) **if** analysis proves observational equivalence.

---

## 5. Design options and trade-offs

Below, “**env**” is the captured state; “**code**” is the code pointer or
function identity.

### 5.1 Flat heap record `{ code*, env* }` (“fat pointer” / object closure)

**Idea:** Allocate a struct `{ tag, fn*, env* }` or `{ fn*, [N x i8] }` for known
small captures; `env` points at boxed fields.

**Pros:** Matches textbook **closure conversion**; easy to document; each
closure instance is self-describing; works with LLVM’s normal loads/stores.

**Cons:** Heap traffic and indirection; needs a **GC map** or conservative
scanning for `env`; many closure **shapes** ⇒ many LLVM types unless erased to a
common layout + interpreted reads (slow) or a table of typed accessors.

**Literature / practice:** Standard ML compilers (SML/NJ, MLton), Scheme, and
Haskell lowering all end up here in some form after **closure conversion**;
Appel–Shao “safe-for-space” variants choose **what** to put in `env` to avoid
retaining dead bindings (see Yale FLINT notes and follow-on work).

### 5.2 Thick pointer `{ data*, invoke* }` (preferred for XL JIT)

(Same broad shape as a Swift **thick function** or a Rust **`Fn`**
trait-object style pair `{ data, vtable / fn pointer }`—we just pin
**`data*`** to boxed **`Scope*`-semantics** for XL.)

**Idea:** Represent a delayed / closure callable as two machine words, e.g.
`{ data*, invoke* }`, where `invoke` is a small code pointer (trampoline,
specialized entry, or shim) and `data*` is the **environment** for that call.

**XL already has the non-boxed analogue.**  Compiled evaluation uses
`eval_fn`, i.e. `Tree *(*eval_fn)(Scope *, Tree *)` (`include/tree.h`): the
first argument is the **lexical environment** as a `Scope *`, the second is the
expression / self tree.  `ClosureAdapter` in `compiler-fast.cpp` passes both
`scopePtr` and the closure tree into that convention.

**Parallel for boxed O3 (design goal):**

| Role | Non-boxed (today’s fast path) | Boxed target |
|------|------------------------------|--------------|
| Code | `eval_fn` | `invoke*` (LLVM function pointer typed in our JIT) |
| Environment | `Scope *` (GC scope chain for lookups) | `data*` = **boxed scope**: machine representation of the *same* lexical object `Scope *` names—bindings / chain the interpreter would use after `IsClosure` |

So **`data*` is not an opaque blob of random captures** (unless we later prove
that equivalent): the intent is that **`data*` is the boxed form of the scope /
environment** that would accompany the thunk in the interpreter, just as
`Scope *` accompanies `eval_fn` today.

**Why JIT weakens the usual “thick function” objections.**  XL uses LLVM in
**JIT** mode for its own generated code: we are not publishing a stable
platform ABI for arbitrary foreign callees.  We can pick a **private** calling
convention for `invoke` (arguments, return type, tail rules) as long as all
call sites and adapters we emit agree.  “Custom calling convention” is then an
internal engineering choice, not a hard compatibility wall.

**Variadic arity and recursive rewrites.**  XL does not need one `invoke` that
accepts a dynamic number of captured slots in the general case: the language
already **splits** variadic surface syntax by **recursive rewrites**, e.g.

```xl
write Head, Rest        is { write Head; write Rest }
```

(`src/builtins.xl`).  Each step matches a **fixed** arity at the IR boundary;
`DoCall`’s lazy-arg peel (`Prefix(Op, Name)` → `Prefix(Op, exp)` when `exp` is
composite) keeps inference aligned with the shaped tree, independent of which
operator name `Op` is.  The same idea applies to closure construction:
**per-shape** `invoke` (and optionally per-shape `data` layout), not one truly
variadic low-level entry.

**Pros:** Clean mapping from interpreter mental model to IR; `invoke` can stay
small and typed; environment layout can evolve with `Scope` lowering.

**Residual cons:** `invoke` may still be **per capture shape** or need a thin
shared dispatcher; **`data*` layout** must be traced by GC if it holds boxed
heap references; nested closures still need an explicit policy (see §7).

### 5.3 Trampoline + shared generic caller (`eval_fn` generalization)

**Idea:** Keep one LLVM function signature `(env*, args…)` and store captured
values in a **homogeneous** `void**` or tagged array; callee reads by index.

**Pros:** Minimal number of LLVM functions; closest to today’s
`ClosureAdapter`.

**Cons:** Loses type information at IR level (harder for LLVM to optimize);
dynamic arity checks; harder to integrate with **typed** O3 rewrites unless
every callee re-checks types.

### 5.4 Defunctionalization / monomorphized “closure kinds”

**Idea:** At compile time, enumerate all closure **creation sites** and represent
the closure as a **small discriminant** + payload struct, with a `switch` at
each **apply** site.

**Pros:** No GC closures for first-order uses; very fast on hot paths.

**Cons:** Code size blow-up; difficult with **separate compilation** / dynamic
loading; painful when closures escape to unknown contexts.

### 5.5 Linked frames (“display” / static chain)

**Idea:** Instead of copying captures, store a pointer to an **activation frame**
chain and compile variable access as static depth + offset (Pascal-style).

**Pros:** Cheap construction when many closures share one frame; classic for
nested functions in Algol-like languages.

**Cons:** Lifetime hazards (**upward funarg**): frames must live as long as any
escaped closure — usually requires heap promotion or heap-allocated frames;
debugging and GC tracing are harder than flat records.

### 5.6 Keep interpreter-style `Tree *` closure inside a boxed wrapper

**Idea:** The boxed machine value is `{ tag = closure, tree* }` where `tree*` is
exactly today’s `Prefix(scope, body)` GC node; slow path uses existing
interpreter or `eval_fn`.

**Pros:** Maximally faithful to current semantics; fastest path to **correct**
behavior for hard cases.

**Cons:** Defeats much of O3’s purpose on those values; mixing typed fast values
with “boxed interpreter thunk” complicates type inference and inlining.

---

## 6. How other implementations and the literature decide

High-level themes (names useful for further reading):

- **Closure conversion** (λ-lifting / phase between typed IR and low-level IR):
  turn functions with free variables into **closed** functions plus **records**
  of captured values.  This is the standard functional-language pipeline; LLVM
  is usually a **lower** layer that just sees structs and pointers.

- **Safe-for-space closure conversion** (Appel, Shao, and successors): refine
  **what** gets copied into each closure record so environments do not retain
  variables after last use — important when closures are allocated frequently.

- **Whole-program vs separate compilation**: MLton-style whole-program analysis
  can merge closure shapes aggressively; incremental compilers (Swift, Rust)
  favor **thick pointers** + per-function private layouts.

- **C++ `std::function` / MSVC `std::bind` history**: type-erased small-object
  optimization + heap fallback — engineering pattern for **uniform** boxed
  callables with painful edge cases (ABI, exceptions, copying).

None of these pick a universal winner in the wider literature; for **XL JIT**,
§5.2 is the best default: thick pointer + **boxed scope as `data*`**, parallel to
`(Scope *, eval_fn)` semantics today.

---

## 7. Recommended direction for XL (working hypothesis)

Align implementation with §5.2 and the **`eval_fn` + `Scope *` ↔ `invoke` +
boxed scope** parallel:

1. **Define one JIT-local “closure value” struct** (LLVM details only in
   `llvm-crap`, per `AGENTS.md`): two words, `{ data*, invoke* }`, with `invoke`
   following a convention we own.  **`data*`** lowers the **same logical
   environment** as interpreter `Scope *` after unwrap (bindings / chain the
   callee would use for lookups)—the boxed **scope** type, not an unrelated tuple
   of captures unless analysis proves equivalence.

2. **Lowering `Scope *` → boxed scope** is then a shared sub-problem: whatever
   O3 uses for “current symbols” in generated code should be the **canonical**
   payload inside `data*` for closure thunks, so closure invocation and normal
   rewrite bodies share one environment story.

3. **Arity and shapes:** follow the **`write Head, Rest`** pattern: recursive
   rewrites and/or per-site monomorphization so each `invoke` sees a **fixed**
   environment layout; avoid a single variadic IR entry unless it is clearly
   better for code size.

4. **Split closure *kinds* only where semantics differ**, not to duplicate the
   environment representation: e.g. tree-delay vs fully typed bodies may still
   use the same **boxed scope** header with different `invoke` targets.

5. **Prove the bridge on `assign.xl`-style examples** (`Items`, caller’s
   `ival`); add regressions for capture width and nesting.

6. **Nested closures:** pick and document one model (`AGENTS.md`)—outer
   `data*` boxed vs raw—and thread it through `Autobox` / `ValueMachineType`.

7. Only after the above, consider **safe-for-space** trimming of what the boxed
   scope retains; do not shrink the environment in ways that break the parallel
   with `Context::Closure` / `IsClosure` without proof.

---

## 8. Code index (for implementers)

| Topic | Location |
|--------|----------|
| `Context::Closure` | `src/context.cpp` |
| `Scope::IsClosure`, `Tree::IsClosure` | `src/tree.cpp`, `include/tree.h` |
| `BindClosure`, `evalLookup`, `Instructions` | `src/interpreter.cpp` |
| Scope chain helpers (`Enclosing`, typedef `Scope`) | `include/context.h` |
| `print Items` builtin | `src/builtins.xl` |
| `eval_fn`, `Scope *` | `include/tree.h` (`typedef … eval_fn`) |
| `treeTy`, `scopeTy`, `evalTy`, `evalFnTy`, … | `src/compiler.cpp` |
| `xl_new_closure`, `ClosureAdapter` | `src/compiler-fast.cpp` |
| `write Head, Rest` (fixed arity per step) | `src/builtins.xl` |
| Stub boxed invoke | `src/compiler-expr.cpp` (`IsClosureType`) |
| Project rules (Autobox, llvm-crap, tests) | `AGENTS.md` |

---

## 9. Further reading (external)

- Andrew W. Appel, *Compiling with Continuations* — CPS pipeline where closures
  are explicit records.
- Zhong Shao and Andrew W. Appel, “Space-efficient closure representations”
  (linked summary page: [Yale FLINT closure representations](http://flint.cs.yale.edu/flint/publications/closure.html)).
- LLVM itself does not mandate a closure model; it receives **lowered** structs,
  pointers, and functions.  Design lives in the **front-end** (XL) and runtime.

---

## 10. Revision history

| Date | Notes |
|------|-------|
| 2026-04-14 | Initial design sketch from interpreter + fast compiler survey. |
| 2026-04-14 | §5.2 preferred: JIT CC, `write` arity split, thick pointer; `data*` as boxed `Scope *` parallel to `eval_fn`. |
| 2026-04-14 | §2.1: `print "ival=", ival` trace (interpreter / fast / O3 LLVM). |
