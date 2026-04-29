# Boxed assignments and reads: design notes for XL (xlr)

This document describes how to make assignment and name reads interoperable
between the interpreter and O3 code generation when values may exist in two
forms:

- as machine values (`i64`, `double`, etc.) in JIT temporaries / storage
- as tree values (`Natural*`, `Real*`, ...) visible through `Context`

The key requirement is:

- keep values unboxed in fast paths as long as possible
- when tree form is needed, write back into the canonical tree node(s)
- preserve interpreter-equivalent semantics for assignment and lookup

---

## 1. Interpreter baseline

### 1.1 Assignment is context mutation

`Context::Assign` (`src/context.cpp`) is the semantic source of truth:

- If a reference does not exist, it creates it with `Define(ref, value)`.
- If it exists, it checks an annotation type if present (`AnnotatedType` +
  `xl_typecheck`), then updates the declaration in place (`decl->right = value`).
- It returns the assigned value.

So `I := 5` means "binding `I` now points at a tree value for 5", and later
`Bound(I)` must observe that value.

### 1.2 Reads resolve from bound trees

`Context::Bound` / `Lookup` return the declaration value (`decl->right`) for a
pattern/name. The interpreter therefore reads from tree state that assignments
mutate.

---

## 2. Current O3 behavior and mismatch

### 2.1 O3 assignment currently writes machine storage only

`CompilerExpression::DoAssignment` (`src/compiler-expr.cpp`) currently:

1. finds existing bound value (`context->Bound(...)`)
2. computes storage type (`ValueMachineType(existing)`)
3. allocates/uses local storage (`NeedStorage(existing, storageType)`)
4. evaluates RHS (`Evaluate(assign->right)`)
5. emits `Store(storage, value)`

This updates compiled storage but not the bound tree node(s) in `Context`.

### 2.2 O3 reads can stay machine-only

`Do(Name*)` prefers compiled-known values (`Known(from)`, globals, etc.), so a
name can be read many times as machine data without touching tree representation.

### 2.3 Why interoperability can fail

If compiled code keeps `I` as `i64` but does not synchronize tree state before a
boundary where tree semantics are observable, the interpreter (or another boxed
path) may still see an old `decl->right`.

---

## 3. Goal: "boxed-late" with explicit materialization

The intended model:

1. A binding may have a fast machine cache.
2. The canonical tree representation is still the observable language state.
3. Materialization ("flush") happens only when needed by tree/interop boundaries.

This is analogous to write-back cache policy:

- fast machine values are dirty cache lines
- tree node(s) are memory visible to interpreter semantics

---

## 4. Binding state model

Add per-binding state in the compiler function for assignable names:

```text
BindingCell:
  patternTree      // declaration base tree (Name or annotated Name)
  rewriteDecl      // Rewrite containing decl->left / decl->right
  scope            // Scope where declaration lives
  machineType      // current fast type (naturalTy, realTy, ...)
  storagePtr       // alloca/global storage used by O3
  treeValuePtr     // canonical tree node if directly mutable (e.g. Natural*)
  dirty            // machine value newer than tree
```

Important distinction:

- `storagePtr` is O3-local optimization state
- `treeValuePtr` / `rewriteDecl->right` is semantic state shared with interpreter

---

## 5. Write path design

### 5.1 Name assignment fast path

For `Name := RHS` in O3:

1. evaluate `RHS` into machine value `v`
2. store `v` into `storagePtr`
3. mark binding cell `dirty = true`
4. return assigned value as today

No immediate tree allocation/update is required.

### 5.2 Flush operation

When flush is required (see Section 7), synchronize to tree state:

1. Load machine value from `storagePtr`.
2. If declaration has type annotation, enforce same cast/check behavior as
   `Context::Assign` before publishing tree value.
3. Materialize or update tree value:
   - **In-place update** when existing `decl->right` has compatible leaf kind:
     - `Natural*`: write `Natural::value`
     - `Real*`: write `Real::value`
     - `Text*`/char case: update text payload consistently
   - **Fallback** otherwise:
     - build boxed tree (`Autobox(..., treePtrTy)`)
     - publish through interpreter-equivalent assignment behavior
       (`Context::Assign` semantics; runtime helper may be used)
4. Update `rewriteDecl->right` if a new tree node was produced.
5. mark `dirty = false`

This keeps direct field updates for hot scalar cases while preserving exact
interpreter semantics for general forms.

---

## 6. Read path design

### 6.1 Default read from machine cache

`Do(Name*)` should continue to use fast storage/known values when available.

### 6.2 Tree-demand read boundary

If the consumer requires tree form (or a call may independently box/read the
binding), enforce `Flush(binding)` before providing a tree observable value.

### 6.3 External mutation / unknown writes

If a path may have assigned via interpreter/runtime without going through the
same compiled binding cell, invalidate local assumptions:

- clear/poison stale cached state, or
- reload from `decl->right` and unbox to machine form before next fast read

Correctness rule: a compiled read cannot silently bypass an intervening
interpreter-visible assignment.

---

## 7. Materialization boundaries ("flush points")

Flush is required before operations where tree-observable state can escape.
Minimum boundaries:

1. **Before capturing for lazy/thick closure values**
   (`ThickClosureForExpr`) when captured names include dirty bindings.
2. **Before passing values as `Tree*` / boxed tree to runtime helpers**
   (`Autobox(..., treePtrTy)` for observable values).
3. **Before calls that may inspect or store in `Context`**
   (including generic runtime/interpreter fallback paths).
4. **Before returning from a compiled region** if caller may continue with
   interpreter evaluation over the same scope/bindings.

Optional optimization: flush only bindings that may be observed by that boundary
instead of flushing all dirty cells.

---

## 8. Assignment/read interoperability example (`I := 5`)

Target behavior:

1. Interpreter semantics says `I` is bound to a `Natural` tree with value 5.
2. O3 may represent `I` as `i64` in storage after assignment.
3. Later, still in same JIT block, `I` reads as `i64` with no boxing.
4. At first tree-observable boundary (e.g. closure capture, runtime call,
   mixed-mode handoff), flush:
   - if `decl->right` is `Natural*`, store `5` to its `value` field
   - else build/update tree value and assign per `Context::Assign` behavior
5. Any interpreter read of `I` now sees 5.

This gives "boxed-late, visible-when-needed".

---

## 9. Implementation sketch by file

### 9.1 `src/compiler-function.h/.cpp`

Add binding-cell helpers:

- `BindingCell *LookupBindingCell(Tree *nameOrPattern)`
- `void MarkBindingDirty(BindingCell &, JIT::Value_p newValue)`
- `void FlushBindingCell(BindingCell &)`
- `void FlushAllDirtyBindings(FlushReason reason)`

Use existing tree layout constants (`NATURAL_VALUE_INDEX`, `REAL_VALUE_INDEX`,
`TEXT_VALUE_INDEX`, etc.) for in-place leaf updates when valid.

### 9.2 `src/compiler-expr.cpp`

- In `DoAssignment`, mark the assignee binding dirty after store.
- In `Do(Name)`, continue fast reads but route tree-demand/escape paths through
  flush-aware helpers.
- Before creating thick lazy closures or other tree-escape artifacts, flush
  referenced dirty bindings.

### 9.3 Runtime bridge (`src/runtime.cpp`) optional helper

If direct in-place update is not applicable, expose a helper that applies
`Context::Assign` semantics from JIT with explicit `(Scope*, ref, valueTree)`.
`xl_assign` already exists and can be reused as the semantic fallback path.

### 9.4 LLVM wrapper layer

Any new LLVM primitive operations required for field stores should remain in
`llvm-crap` wrappers, per project layering rules.

---

## 10. Scope and rollout

### 10.1 Phase 1 (name scalars)

Support:

- assignee is plain `Name`
- value kinds natural/real/boolean/text where machine<->tree is straightforward

Fallback all other shapes through `xl_assign`.

### 10.2 Phase 2 (structured values)

Extend to tuples/forms where "corresponding tree node or nodes" means writing
multiple fields/subtrees consistently, still with interpreter-equivalent publish
at boundaries.

### 10.3 Phase 3 (optimization)

Minimize flush frequency using escape/use analysis:

- per-boundary affected-set flushing
- avoid re-flush when not dirty
- preserve fast local loops

---

## 11. Test plan

Add/refresh tests that mix O3 and interpreter-observable reads:

1. `I := 5` in O3, then interpreter-visible read in same scope.
2. O3 assignment followed by closure capture and delayed interpreter use.
3. Reassignment sequence (`I := 1; I := 2`) with only final value published.
4. Typed assignment (`I:natural := ...`) with mismatch path matching
   `Context::Assign` behavior.
5. Mixed fallback case where O3 must call `xl_assign`.

Success criterion: no divergence between `-i` and `-O3` observable assignment
results at language boundaries.

---

## 12. Recommended default policy

Use a write-back policy:

- keep machine values local by default
- flush only at explicit tree-observable boundaries
- use in-place leaf updates when safe
- otherwise delegate to `Context::Assign` semantics

This gives O3 performance while preserving one authoritative language behavior
for assignment and lookup.

---

## 13. Code index

| Topic | Location |
|------|----------|
| Assignment semantics | `src/context.cpp` (`Context::Assign`) |
| Bound lookup semantics | `src/context.cpp` (`Context::Bound`, `Lookup`) |
| Assignment detection helpers | `include/context.h` (`IsAssignment`) |
| O3 assignment codegen | `src/compiler-expr.cpp` (`DoAssignment`) |
| O3 name reads | `src/compiler-expr.cpp` (`Do(Name*)`) |
| Thick closure build/invoke | `src/compiler-function.cpp` (`ThickClosureForExpr`, `InvokeThickClosure`) |
| Autobox/unbox | `src/compiler-function.cpp` (`Autobox`) |
| JIT tree type layouts | `src/compiler.cpp`, `src/compiler.h` |
| Runtime assignment bridge | `src/runtime.cpp` (`xl_assign`) |

---

## 14. Revision history

| Date | Notes |
|------|-------|
| 2026-04-29 | Initial design sketch for boxed assignment/read interoperability with explicit flush boundaries. |

