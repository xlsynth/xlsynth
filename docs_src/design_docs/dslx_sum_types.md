# DSLX Sum Types Design

## Goal

Add first-class semantic sum types to DSLX with a coherent declaration, matching, interpreter,
and lowering story.

## Phase 1 (MVP)

The first landing is intentionally narrow. Phase 1 includes only:

- sum-type declarations and constructors in the language.
- basic `match` over declared constructors.
- `==` and `!=` support for sum values.
- DSLX interpreter support for those Phase 1 features.
- lowering sum values to IR as product types, using tuples.
- `zero!<MySum>()` type errors for sum types.

## Phase 2 (Next Steps)

After Phase 1, the next step is to make the implementation more efficient and wrap up a few
good-to-haves:

- boundary decode semantics plus malformed-value behavior for external wire inputs.
- format support.
- `invalid!` support.
- sum-aware `if let` sugar.
- move constructor evaluation, interpreter/runtime storage, and IR lowering from the Phase 1
  tuple-product form to one shared tagged-union representation based on `tag_bits ++ payload_slot`.
- generate wire-compatible tagged-union SystemVerilog types so the DSLX-to-SystemVerilog interface
  can use packed unions.

## Phase 3 (Follow-on)

After Phase 2, the next compiler-internal cleanup is to replace the MVP sum exhaustiveness checker
with a recursive representation that matches the semantic structure of sums, tuples, and structs.

The detailed sections below explain the Phase 1 and Phase 2 items, and the document also records
follow-on directions and explicit non-goals where that clarifies the intended boundary of the
design.

## Scope

This document covers the Phase 1 MVP and the Phase 2 next steps. It also records follow-on
directions and explicit non-goals so the intended rollout boundary is clear.

## Rollout Map

| Section | Phase | Notes |
| --- | --- | --- |
| `Declaration Syntax`, `Constructor Reference Syntax`, basic constructor `match`, well-formed equality, interpreter support, Phase 1 tuple lowering, `Semantic Model`, and `zero!` | Phase 1 | Core language semantics and interpreter/compiler work needed for the MVP. |
| `High-Level TIv2 Model`, `Parametric Sums`, `Constructor Typing`, declared-constructor pattern typing, `Exhaustiveness and Redundancy`, `TIv2 Integration Points`, `Sum Declaration Parsing`, and `Optimization Considerations` | Phase 1 | Core type-system, parser, and optimizer work for the MVP. |
| `Malformed-Boundary Extension`, `Later Wire-Compatible Representation`, `Malformed Values`, `Formatting and Tracing`, and `if let` | Phase 2 | User-visible boundary semantics, malformed-value behavior, source-level sugar, and the shift from the Phase 1 tuple-product internals to one shared tagged-union representation. |
| `Malformed Pattern Parsing`, `invalid!` pattern typing, `TIv2 Integration Points for Malformed Values`, and `Lowering Design for Malformed Values and if let` | Phase 2 | Parser, TIv2, runtime, and lowering work needed to replace the Phase 1 tuple representation with the Phase 2 shared tagged-union path and to implement the malformed-input and `if let` surfaces. |
| `Recursive Exhaustiveness Representation` | Phase 3 | Compiler-internal replacement for the MVP sum exhaustiveness checker so coverage, redundancy, and uncovered-example synthesis operate on constructor-aware recursive spaces instead of one flattened MVP region. |

## Explicit Non-Goals

- No direct variant field access is included in Phase 1 or Phase 2. Any later projection must require proof that a specific constructor is the chosen one.
- No refutable bindings are planned; pattern failure must remain explicit in `if let` or `match`.
- No `impl` support for sum types is included in Phase 1 or Phase 2. Whole-sum `impl` support is planned as a follow-on.
- No compatibility spelling keeps `enum Name { ... }` alive for sums after `sum` lands.
- No requirement that programmers handle malformed tags explicitly in all code.

## Declaration Syntax (Phase 1)

This design uses `sum` for semantic sums and leaves `enum` as the numeric-enum keyword.

- `enum MyEnum { ... }` remains the inferred-width numeric enum form.
- `enum MyEnum : u8 { ... }` remains the explicit underlying-type numeric enum form.
- `sum MySum { ... }` declares a sum type.

This keeps numeric-enum spelling and semantics intact while giving semantic sums an explicit
declaration keyword. The `sum` introducer is contextual at declaration sites, so ordinary
identifiers named `sum` remain valid outside sum definitions. There is no compatibility support
for `enum MySum { ... }` after this change.

Illustrative syntax:

```dslx
sum MySum {
  Foo,
  Bar(u8, bool),
  Baz { x: u8, y: u16 },
}
```

Phase 1 supports all three variant forms:

- Unit variants
- Tuple variants
- Struct variants

Visibility follows the existing nominal-type model:

- A sum definition may be `pub` or private.
- Constructors do not have independent visibility modifiers.
- Every constructor inherits the visibility of its enclosing sum.

Illustrative forms:

```dslx
sum MySum {
  UnitCase,
  TupleCase(u8, bool),
  StructCase { x: u8, y: u16 },
}
```

## Constructor Reference Syntax (Phase 1)

This design uses qualified constructor references by default:

```dslx
MySum::UnitCase
MySum::TupleCase(u8:1, true)
MySum::StructCase { x: u8:1, y: u16:2 }
```

The same qualification is used in patterns:

```dslx
match x {
  MySum::UnitCase => a,
  MySum::TupleCase(v, b) => f(v, b),
  MySum::StructCase { x, y } => g(x, y),
}
```

This applies in expressions and `match`. Phase 2 `if let` reuses the same qualified constructor
syntax.

Parametric sums use the same explicit parametric syntax as existing DSLX nominal types. When
explicit sum parametrics are written, the spelling is `Sum<...>::CaseName` in expressions and
patterns.

```dslx
sum Option<T: type> {
  None,
  Some(T),
}

fn pattern_example(a: Option<u32>) -> u32 {
  match a {
    Option::Some(v) => v,
    Option<u32>::None => u32:0,
  }
}
```

In normal code, constructor expressions and patterns remain inference-first:

- `Option::Some(u32:7)` infers `T = u32`.
- `Option::None` usually needs surrounding type context.
- `Option<u32>::None` is the explicit fallback when inference cannot determine the instantiated sum type.

This design does not add a DSLX-specific scrutinee-inferred pattern shorthand. Constructors are
written as `SumName::CaseName` when parametrics are omitted and `SumName<...>::CaseName` when
explicit parametrics are written. The explicit arguments attach to the sum type name, not to the
constructor, so `Option::Some<u32>(...)` and `Option::<u32>::Some(...)` are not part of this
design.

If a future DSLX `use` feature can bring constructor names into scope so code can write bare
constructors such as `Some(x)`, that is orthogonal to sum types and is not specified here.

## Basic `match` over Declared Constructors (Phase 1)

On a well-formed sum-typed runtime value, ordinary constructor patterns match the declared
constructors.

`_` means all remaining well-formed constructors.

Illustrative form:

```dslx
match x {
  MySum::Foo => a,
  MySum::Bar(v) => b(v),
  _ => c,
}
```

Later sections add malformed-input behavior, `invalid!`, and the boundary rules that define when a
runtime sum value is malformed.

## Equality and Inequality on Well-Formed Values (Phase 1)

For two well-formed operands of the same instantiated sum type, equality is semantic:

- The constructors must be the same.
- The payloads are compared using ordinary DSLX equality for the payload shape of that constructor.
- Ignored high payload bits do not participate in equality. Two valid raw wire images that differ only in ignored high payload bits above the selected constructor's payload width compare equal.

Inequality is defined as the logical negation of equality.

Later sections add malformed-input behavior for these operators.

## DSLX Interpreter Support (Phase 1)

The interpreter support for the MVP is:

- evaluating constructor expressions to semantic sum values
- selecting `match` arms by constructor and binding payload values
- evaluating `==` and `!=` by comparing constructor and payload semantically

Later sections describe malformed boundary values and the additional observer behavior that comes
with them.

## Phase 1 IR Lowering (Phase 1)

The MVP lowers sum values to ordinary IR product types using tuples.

A lowered Phase 1 sum carries:

- the selected constructor tag
- the flattened payload slot for that constructor

Construction, case analysis, and equality can therefore be lowered through ordinary tuple
construction and deconstruction plus the flat-bit operations already used elsewhere in the design.
This is a compiler strategy rather than a user-visible semantic commitment.

## Semantic Model (Phase 1)

Each sum type has a semantic value space consisting only of its declared constructors and their
payloads.

Phase 1 features such as constructor expressions, `match`, equality, interpreter evaluation, and
tuple lowering operate only over that semantic value space.

## `zero!<MySum>()` (Phase 1)

`zero!<MySum>()` is a type error.

Rules:

- `zero!` does not construct sum values.
- If the explicit type argument names a sum type, typechecking fails.

This follows the existing DSLX precedent that `zero!` is only available when the target type has a
well-defined zero value. In particular, DSLX already rejects `zero!` on enums that do not have a
member with literal value zero.

## Malformed-Boundary Extension (Phase 2)

Phase 2 extends the runtime model beyond the Phase 1 semantic value space and replaces the Phase 1
tuple-product internal representation for sum construction, interpreter storage, equality, and
lowering with one shared tagged-union representation.

Within that Phase 2 representation, a runtime sum value may be:

- a well-formed semantic constructor value
- a malformed value that carries the full raw wire image unchanged

The following sections define the shared wire-compatible representation, the boundary decode rule,
and the observer behavior for those malformed values.

## Later Wire-Compatible Representation (Phase 2)

The following sections define the Phase 2 shared tagged-union representation used by constructor
evaluation, interpreter/runtime storage, equality, and lowering, as well as by SystemVerilog
correspondence and malformed-value handling.

### Tag Width and Numbering

`TAG_W` is determined exactly by the constructor count:

- `TAG_W = ceil_log2(constructor_count)`
- For a sum with exactly one constructor, `TAG_W = 0`.

Constructor declaration order defines wire tag numbers.

- The first declared constructor has tag `0`.
- Reordering constructors is therefore an ABI change.

### Payload Layout

Each variant payload is flattened into a shared payload slot.

Rules:

- Payloads are low-bit aligned within the shared payload slot.
- For a valid constructor tag, only the low payload bits within that constructor's flattened payload
  width participate in decode.
- Payload bits above the selected constructor's flattened payload width are ignored by decode and do
  not make the value malformed.
- Unused high payload bits are zero-filled for DSLX-generated valid encodings.
- Struct variant payload flattening follows normal DSLX struct/tuple flattening rules.

Example:

```text
wire_sum = tag_bits ++ zero_ext(flatten(payload), MAX_PAYLOAD_BITS)
```

This representation is not boundary-only. In Phase 2, well-formed constructor expressions and
internal lowering paths materialize the same `tag_bits ++ payload_slot` layout instead of the Phase
1 sibling-expanded tuple-product form. Raw boundaries use this same layout for decode and
canonical re-encode, while malformed values preserve the full raw image unchanged as described
below.

### Raw Representation Type

The full raw wire image has type:

```text
bits[TAG_W + MAX_PAYLOAD_W]
```

This is the type bound by `invalid!(raw)`.

In Phase 2, there is no user-spellable raw sum type and no explicit encode/decode API. Raw wire images
enter sum semantics only at external boundaries that traffic in this representation. `invalid!(raw)`
is the only surface that binds the full malformed raw wire image as a `bits[TAG_W + MAX_PAYLOAD_W]`
value after boundary decode. In generated RTL/Verilog and synthesis, when no explicit `invalid!`
arm is present, the final constructor arm may also project malformed payload bits through that
constructor's payload shape as described in `Last-Arm Fallback Payload Binding`, but that path does
not expose the full raw image.

### External Inputs and SystemVerilog Correspondence

DSLX functions may be fed by SystemVerilog or other external hardware that does not guarantee every
wire-level tag/payload combination corresponds to a declared semantic value.

`Boundary Decode Semantics` defines how those external wire images become well-formed or malformed
sum values.

The later wire-compatible representation is intentionally chosen so that it can be represented in
SystemVerilog with a wire-compatible tag-plus-payload encoding, using the usual packed-enum and
packed-union idiom.

Conceptually, the corresponding SystemVerilog side is:

- when `TAG_W > 0`, a packed enum for the tag
- a packed union for the payload alternatives
- for singleton sums, no tag field is present and the wire image is just the payload slot
- or, equivalently, a flat packed vector with the same `tag_bits ++ payload_slot` layout

This is wire compatibility, not semantic equivalence:

- the DSLX semantic sum contains only declared constructors
- the SystemVerilog packed representation admits all bit patterns, including malformed tag/payload
  combinations

## Malformed Values (Phase 2)

Earlier sections define declarations, basic `match`, equality, and formatting for well-formed sum
values. This section adds the raw-boundary decode rule and the malformed-value behavior of those
observers.

### Boundary Decode Semantics

When raw wire bits from the Phase 2 shared tagged-union representation are interpreted as a
sum-typed runtime value, the decode rule is:

1. Split the raw wire image into `tag_bits` and `payload_slot`.
2. Interpret `tag_bits` as a constructor tag for the enclosing sum type. When `TAG_W = 0`, `tag_bits` is empty and the sole constructor is implied.
3. If `tag_bits` do not correspond to any declared constructor, produce a malformed runtime sum value that carries the full raw wire image unchanged.
4. Otherwise, select the declared constructor named by that tag.
5. Let `W` be the flattened payload width of that selected constructor.
6. Decode the constructor payload from the low `W` bits of `payload_slot`, using ordinary DSLX tuple and struct flattening rules for that payload shape.
7. Ignore all bits above `W` in `payload_slot`; they do not contribute to the semantic value and do not make the input malformed.
8. Produce a well-formed runtime sum value containing the selected semantic constructor and decoded payload. For a unit constructor, this is just that constructor with no payload.

Consequences:

- Malformed runtime values arise exactly when the wire tag does not name any declared constructor.
- For a valid constructor tag, decode is many-to-one: raw wire images that differ only in ignored high payload bits above the selected constructor's payload width denote the same well-formed semantic sum value.
- DSLX-generated valid encodings use the canonical all-zero choice for those ignored high payload bits.
- Sum-typed boundaries are semantic and canonicalizing, not raw-bit-preserving: a valid raw wire image may decode to a semantic sum value and, at the next sum-typed output boundary, re-encode to a different but equivalent canonical wire image if the original differed only in ignored high payload bits.
- Well-formed constructor evaluation and internal transport use the same canonical
  `tag_bits ++ payload_slot` layout, so constructors and lowering do not materialize inactive
  sibling payloads as separate internal fields.
- Example:

  ```dslx
  enum Msg {
    Small(u4),
    Big(u8),
  }

  fn passthrough(x: Msg) -> Msg {
    x
  }
  ```

  If `Small` has tag `0`, then the raw inputs `0 ++ 00001010` and `0 ++ 11111010` both decode to `Msg::Small(u4:0b1010)`. `passthrough` preserves that semantic value, but when the result is emitted at a sum-typed output boundary the canonical valid encoding is `0 ++ 00001010`, so the second input does not round-trip bit-for-bit.
- The malformed-value observers in Phase 2 are `match`, `==`, `!=`, and formatting/tracing. Within
  `match`, `invalid!(raw)` exposes the full raw wire image. Separately, the generated-RTL/Verilog
  and synthesis final-arm fallback may project malformed payload bits through the final
  constructor pattern when no explicit `invalid!` arm is present. Other operations that transport
  sum values without inspecting constructors preserve malformed runtime values unchanged.

### `match` and `invalid!`

`match` gains a special top-level malformed-input arm:

```dslx
invalid! => expr
invalid!(raw) => expr
```

Rules:

- At most one `invalid!` arm may appear in a `match`.
- It is only valid when matching on a sum type.
- It is only valid as a top-level arm pattern.
- It cannot participate in `|` alternatives.
- `invalid!(raw)` binds the full raw wire representation, including tag and payload bits.

### Arm Ordering

In a sum-aware `match`, `invalid!` or `invalid!(raw)` may appear only as the final arm.

If `_` appears, it may be followed only by that final `invalid!` arm.

This preserves the current intuition that `_` ends the valid-case list, while still allowing the explicit malformed-input case to appear last.

Illustrative form:

```dslx
match x {
  MySum::Foo => a,
  MySum::Bar(v) => b(v),
  _ => c,
  invalid!(raw) => d(raw),
}
```

### Malformed Input Behavior

DSLX/interpreter simulation behavior:

- Every sum-aware `match` in DSLX/interpreter simulation gets an injected well-formedness assert on its scrutinee before arm selection.
- If that assert fails, the `match` fails immediately.
- This is true even if an `invalid!` arm is present.
- The purpose of this source-level simulation is to expose and fix such conditions, not to mask them.

Generated RTL/Verilog simulation behavior:

- After XLS lowers the design and emits hardware, DV may drive malformed raw inputs into the generated RTL/Verilog.
- In that emitted-hardware simulation context, malformed-input handling is observable through the lowered logic for `invalid!` and the final-arm fallback rules below.
- If an `invalid!` arm is present, malformed input selects that arm.
- If no `invalid!` arm is present, malformed input selects the final non-`invalid!` arm.

Synthesis behavior:

- Synthesis preserves the same malformed-input selection rules as the generated RTL/Verilog.
- If an `invalid!` arm is present, malformed input selects that arm.
- If no `invalid!` arm is present, malformed input selects the final non-`invalid!` arm.

### Last-Arm Fallback Payload Binding

The generated-RTL/synthesis last-arm fallback is defined as follows:

- It applies only when no explicit `invalid!` arm is present.
- The fallback is not ordinary constructor matching and does not make the malformed input a
  well-formed semantic constructor value.
- The final arm must contain exactly one top-level pattern.
- The final arm must be either `_` or a constructor pattern whose payload subpatterns are irrefutable.
- Here "irrefutable payload subpatterns" means the payload pattern cannot fail once the constructor is fixed, for example name bindings, wildcards, and tuple/struct nests composed only of such subpatterns.
- If the final arm is `_`, the arm body is selected directly.
- If the final arm is a constructor pattern, the malformed raw wire image is projected through
  that constructor's payload shape using the payload rules already defined in `Boundary Decode
  Semantics`. The resulting projected payload then binds the payload subpatterns normally.
- This projection may expose malformed payload bits indirectly through the final constructor arm,
  but only `invalid!(raw)` binds the full raw wire image.

This is intentionally permissive for non-defensive code. Programmers who want stronger malformed-input control should write an explicit `invalid!` arm.

### Equality and Inequality

Simulation behavior:

- Equality and inequality on sum operands in simulation get injected well-formedness asserts on both operands before comparison.
- If either assert fails, evaluation fails immediately.

Synthesis behavior:

- If both operands are well formed, use semantic equality.
- If exactly one operand is malformed, equality is `false`.
- If both operands are malformed, equality compares the full raw wire image bitwise, including all tag bits and all payload-slot bits.
- Because malformedness in Phase 2 is defined only by invalid tags, a well-formed value and a malformed value can never have the same raw wire image.
- For singleton sums with `TAG_W = 0`, malformed values cannot arise.

Inequality is defined as the logical negation of equality.

## Formatting and Tracing (Phase 2)

### Well-Formed Values

For a well-formed sum operand, formatting is semantic rather than raw-bit-based:

- the formatted text identifies the selected constructor
- tuple-variant payloads print using tuple-style payload formatting
- struct-variant payloads print using struct-style payload formatting
- unit variants print as just the constructor name
- format preferences such as default, hex, and binary apply recursively to the payload values, not to the hidden raw wire encoding

Illustrative shapes:

```dslx
MySum::Foo
MySum::Bar(u8:42)
MySum::Baz { x: u8:1, y: u16:2 }
```

### Malformed Values

Malformed formatting and tracing are defined only for the existing trace-oriented
simulation/interpreter surfaces such as `trace!`, `trace_fmt!`, and `vtrace_fmt!`.

Malformed sum operands on those surfaces get the usual fail-fast behavior:

- Formatting and tracing on sum operands get an injected well-formedness assert on every sum operand before text is produced.
- If that assert fails, evaluation fails immediately.
- No special printed `invalid` form is produced in Phase 2.

## `if let` (Phase 2)

`if let` is syntactic sugar for `match` on sum values.

It does not have independent malformed-input behavior. When code needs explicit malformed-input
handling, it should use `match` directly.

```dslx
if let P = x {
  a
} else {
  b
}
```

desugars to:

```dslx
match x {
  P => a,
  _ => b,
}
```

`else if` chains are lowered by applying the same rewrite recursively inside the `_` arm. That
may produce nested `match` expressions, which is acceptable.

For example:

```dslx
if let MySum::Pat(a) = m {
  f(a)
} else if let OtherSum::PatZ(z) = n {
  g(z)
} else {
  h()
}
```

desugars to:

```dslx
match m {
  MySum::Pat(a) => f(a),
  _ => match n {
    OtherSum::PatZ(z) => g(z),
    _ => h(),
  },
}
```

Restrictions:

- A chain may contain multiple `if let` clauses.
- Malformed behavior comes entirely from the lowered `match` expressions.
- In generated RTL/Verilog simulation and synthesis, malformed input on a scrutinized sum value flows to the final arm of the corresponding lowered `match` when no explicit `invalid!` arm exists.
- In simulation, the corresponding lowered `match` asserts well-formedness before arm selection.

## Type System Integration

This area spans both committed phases, so the subsection headings carry the rollout tags directly.

### High-Level TIv2 Model (Phase 1)

- A sum is a new nominal DSLX type kind, alongside structs and enums.
- The AST needs a `SumDef` and variant-definition nodes.
- `TypeDefinition` and `TypeRefTypeAnnotation` must be able to name a `SumDef`, just as they do for other nominal types today.
- The concrete `Type` hierarchy needs a `SumType`.
- `SumType` is nominal. Two sum types are equal only when they refer to the same `SumDef` with the same instantiated parametric arguments.
- Variants are constructors of the enclosing sum type; they do not introduce standalone types of their own.

### Parametric Sums (Phase 1)

- Sums use the same parametric binding syntax as structs, functions, and procs.
- This design supports both value parametrics and type parametrics.
- Variant payload types may mention the sum's parametric bindings.
- Explicit parametrics conceptually belong to the sum type, not to an individual variant declaration.
- Constructor expressions and constructor patterns operate on instantiated sum types.
- The explicit surface spelling and everyday inference behavior are defined in `Constructor Reference Syntax`; this section focuses on the typing, inference, and name-resolution consequences of that surface syntax.
- Constructor expressions and constructor patterns are inference-first. This section does not restate the surface examples; it only records the type-system consequences of that inference model.
- If required parametrics remain unsolved after considering payloads, scrutinee type, and surrounding expected type, typechecking fails.
- Alias and import resolution for sums follows the same nominal-type model as structs and enums: aliases are transparent, imported qualified names must resolve through public nominal definitions, constructors inherit visibility from their enclosing sum, and explicit parametric arguments propagate through the unwrapped nominal type reference.

### Constructor Typing (Phase 1)

- `SumName::CaseName` denotes a constructor of a specific instantiated sum type.
- A unit constructor in expression position has the enclosing sum type.
- A tuple constructor application has the enclosing sum type, and each argument is typed against the corresponding declared payload element type after substituting the instantiated sum parametrics.
- A struct constructor application has the enclosing sum type, and each named field is typed against the corresponding declared field type after substituting the instantiated sum parametrics.
- In this design, constructor forms are special construction syntax, not first-class function values.
- For example, `MySum::Foo(v)` and `MySum::Bar { x, y }` construct semantic sum values, but bare `MySum::Foo` is not an ordinary expression of function type in Phase 1.
- This mirrors current struct construction syntax, where `S { x: u8:1 }` is special construction syntax and `S` is not a first-class constructor function.
- If DSLX later gains broader user-defined higher-order-function support, constructor references could in principle be generalized to function-valued constructors, for example `MySum::Foo : u8 -> MySum`.
- This design does not require or assume that generalization.
- Constructor expressions produce semantic sum values. They do not produce raw wire values.
- The malformed-wire space is not modeled as an extra semantic constructor.
- Arm expression typing is otherwise unchanged: every arm expression of a sum-aware `match`, including an `invalid!` arm, must unify to the type of the whole `match`.

### Pattern Typing for Declared Constructors (Phase 1)

- The scrutinee of a sum-aware `match` must have a sum type.
- A constructor pattern is typechecked against one declared constructor of that instantiated sum type.
- Payload subpatterns are typechecked recursively against that constructor's payload shape.
- Nested payload patterns may use the ordinary pattern language for the payload type.
- Phase 1 temporarily restricts each declared payload member type to bits-like
  and enum types. Aggregate payload member types such as structs, tuples,
  arrays, and nested sums are deferred until Phase 2.
- `_` remains a valid-constructor wildcard only; it never binds or implies the raw representation.
- Phase 2 `if let` reuses the same sum-scrutinee and constructor-pattern typing rules.

### `invalid!` Pattern Typing (Phase 2)

- `invalid!` and `invalid!(raw)` are only legal as top-level patterns on a sum scrutinee.
- `invalid!(raw)` binds the raw representation type described above, not a semantic sum value.

### Exhaustiveness and Redundancy (Phase 1)

- Exhaustiveness for sum matches is defined over the declared semantic constructors only.
- When Phase 2 adds `invalid!`, it does not contribute to semantic exhaustiveness.
- A sum match is exhaustive when its constructor patterns together with `_`, if present, cover all declared constructors of the instantiated sum type.
- The generated-RTL/synthesis malformed-input fallback to the last arm, when no `invalid!` arm is present,
  does not affect exhaustiveness or redundancy checking. It is a post-match fallback rule, not
  semantic coverage of an additional constructor case.
- Parametric payload types do not change the coverage rule; coverage is by constructor.
- Constructor arity mismatches, tuple-width mismatches, missing struct fields, and unknown struct fields are type errors, not exhaustiveness issues.
- Redundant constructor arms should be diagnosed analogously to today's redundant enum and tuple-pattern arms.
- The current bits-and-enum-only match exhaustiveness checker is not sufficient for sums; sums require constructor-aware exhaustiveness checking.
- Phase 3 should replace the MVP flattening-based sum checker with the recursive representation
  described in `Recursive Exhaustiveness Representation (Phase 3)` instead of extending the MVP
  flattener further.

### TIv2 Integration Points (Phase 1)

- TIv2 currently treats nominal types as an enum case or a struct/proc case. Sum types add a third nominal case throughout nominal resolution, unification, concretization, and validation.
- The current struct-specific nominal helpers should be generalized into nominal-type helpers so parametric instantiation and payload-type substitution can work for sums as well as structs.
- The current pattern AST and pattern typing logic are `NameDefTree`-centric and are not sufficient for constructor-with-payload sum patterns. Sum patterns require dedicated AST representation and TIv2 handling.
- `zero!<Sum<...>>()` is always a type error.

### TIv2 Integration Points for Malformed Values (Phase 2)

- `invalid!` is a match-pattern feature, not a new semantic value in `SumType`. TIv2 should type it as a special pattern form with a raw-bits binding when requested.
- TIv2 should preserve the malformed-value model defined in `Boundary Decode Semantics`, including which operations observe malformedness and which ones merely transport it.

## Follow-on Design (Follow-on)

These sections record intended post-MVP directions. They are not part of the committed Phase 1 or
Phase 2 rollout.

### Recursive Exhaustiveness Representation (Phase 3)

The ideal sum exhaustiveness engine is recursive and constructor-aware. Leaf types such as bits and
enums stay in interval-style spaces, tuples and structs are products of their member spaces, and a
sum is represented as a partition by declared constructor where each constructor owns only its own
payload coverage space. Pattern subtraction, redundancy checks, and uncovered-example generation
all recurse over that semantic structure, so matching `Foo::Bar(...)` only touches the `Bar`
branch and never allocates wildcard dimensions for inactive constructors.

This Phase 3 engine explicitly replaces the current MVP implementation rather than extending it.
The MVP checker flattens a sum into one region containing the tag plus the payload leaves of every
variant, which is acceptable for the narrow Phase 1 rollout but ties compile-time cost to the full
payload surface of the sum and mixes inactive-payload bookkeeping into active-constructor coverage.
Phase 3 deletes that flattened representation and moves to one recursive coverage model that is
both more robust and more faithful to the surface `match` semantics, especially once aggregate
payloads and nested sums are common.

### Planned `impl` on Sums

Whole-sum `impl` support is planned, but it is not part of Phase 1 or Phase 2.

Sum types use the same associated-member model as structs: a sum may have a single `impl`
containing associated consts, static functions, and instance methods. Inside an instance method,
`self` has the sum type, and variant-specific behavior is expressed with `match` or `if let`
rather than direct field access.

Planned rules:

- Do not allow per-variant impls such as `impl Sum::Case`.
- Keep methods and constructors in separate namespaces logically, but reject name collisions so
  `Sum::Foo` is never ambiguous between a constructor and an associated item.
- Methods operate on semantic sum values only. `invalid!` does not participate in methods on
  `Self`; malformed-wire handling remains boundary and match-pattern behavior, not method
  dispatch.

Rationale:

- `impl` is already the nominal associated-item mechanism in DSLX for structs.
- Sum types are also nominal types.
- This gives sums a natural home for operations such as `is_foo`, `map`, `unwrap_or`, and
  associated constants without introducing any special-case namespace or method system.
- Keeping `impl` out of the MVP reduces the initial landing surface while still recording the
  intended end-state for follow-on work.

### Deriving on Sums

This design does not support `#[derive(...)]` on sum types in Phase 1 or Phase 2.

Recommended rules:

- Reject `#[derive(...)]` on sum-type `enum` declarations.
- Do not add any sum-specific builtin derive behavior in Phase 1 or Phase 2.
- If sum traits are needed later, add them explicitly in a follow-on design instead of leaving
  partial or inferred derive semantics in the initial feature.

Rationale:

- Current DSLX derive support is already narrower than a general trait system.
- Sums need their own trait semantics for constructor-sensitive behavior, exhaustiveness-shaped
  operations, and interaction with nominal matching.
- Keeping the initial rollout explicit avoids committing to trait and derivation behavior before the sum type
  surface itself has shipped and stabilized.

### Direct Variant Field Access (Follow-on)

This design does not support direct field access for struct variants in Phase 1 or Phase 2.

However, unlike refutable bindings, this is not ruled out forever. A later follow-on design may
consider guarded variant projection, but only in scopes where the language has a proof that a
specific constructor is the chosen one.

For example, after control flow has proven `v` is `MySum::Foo`, a future design could consider a
projection form on `v` inside that narrowed scope. This document does not commit to any such
surface syntax, typing rule, or malformed-value interaction; it only records the admissibility
boundary for later work.

Outside a proven-constructor scope, sum values remain nominal and payload access stays
pattern-based.

This keeps sum values nominal and avoids introducing partially-valid projection behavior for values
that may have malformed raw origins.

## Permanent Non-Goals

These sections record surfaces that this proposal intentionally does not plan to add. They are not
deferred Phase 1/Phase 2 work and are not follow-on roadmap items for this design.

### Refutable Bindings (Not Planned)

Refutable bindings are not planned for this design.

In particular, this design does not add refutable `let`, and it does not defer refutable bindings
to a later phase of this proposal.

DSLX is an expression-oriented hardware DSL with little to no notion of side-effectful statement
sequencing. Control flow is intended to be explicit in constructs such as `if` and `match`, so the
dataflow they imply is visible in the source. A refutable `let` would hide a control-flow split
inside a binding form and would require implicit failure semantics, an `else` attachment, or some
other non-local rule to explain what happens when the pattern does not match. Requiring refutable
patterns to appear in `if let` or `match` keeps the branching structure explicit and better fits
DSLX's dataflow-oriented design.

Allowed:

```dslx
if let MySum::Foo(x) = y {
  ...
} else {
  ...
}
```

Not supported by this design:

```dslx
let MySum::Foo(x) = y;
```

## Parser Design

This area spans both committed phases, so the subsection headings carry the rollout tags directly.

### Sum Declaration Parsing (Phase 1)

- The scanner remains unchanged.
- After `enum Name`, the parser distinguishes the two declaration forms with one token of
  lookahead: `:` keeps the existing numeric-enum form, while `{` introduces a sum type.

### Malformed Pattern Parsing (Phase 2)

- `invalid!` and `invalid!(raw)` are parsed as special pattern forms, not as ordinary identifiers.

## Lowering Design for Malformed Values and `if let` (Phase 2)

- Phase 2 lowering uses one shared tagged-union representation for constructor evaluation,
  well-formed runtime transport, equality, `match`, and boundary crossings:
  `tag_bits ++ payload_slot` for well-formed values, and the full raw wire image when preserving a
  malformed value.
- Do not keep the Phase 1 tuple-product representation behind adapters for constructor or lowering
  paths. Replacing that path with the shared tagged-union layout is part of the Phase 2 design, not
  an optional implementation choice.
- `if let` becomes a distinct sum-aware syntactic form with dedicated lowering to `match`.

## Optimization Considerations (Phase 1)

The Phase 1 feature set is expected to fit reasonably well into the existing optimizer.

The lowered representation is expressed in terms of ordinary tuple construction and
deconstruction plus flat-bit operations, so optimization passes can reason about sums by looking
through the same primitives they already understand:

- tuple construction
- tuple indexing
- bit-slice operations
- concat operations

Similarly, `match` and related conditional structure lower into forms that existing conditional
optimization machinery can already analyze. In particular, conditional specialization and related
passes should handle many sum-driven `match` and conditional cases without requiring a separate
sum-specific optimization framework. Phase 2 `if let` desugaring reuses that same `match`-driven
optimization story by lowering into nested `match` expressions.
