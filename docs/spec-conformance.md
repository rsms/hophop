# HopHop Spec Conformance Matrix

This document maps rule families in [`docs/language.md`](./language.md) to existing tests.

Conventions:
- `Core` means required for Stable-rule conformance.
- `Ref` means Reference-hop profile coverage for Provisional/implementation-defined behavior.
- Exact stderr strings in current tests are treated as reference artifacts, not normative requirements.

## 1. Core Coverage Matrix

| Rule family | Profile | Primary evidence tests |
|---|---|---|
| `LEX-*` (tokens, comments, semicolon insertion baseline) | Core | `_:tests/basic.hop`, `_:tests/bad_string.hop`, `ast:tests/ast_basic.hop` |
| `SYN-*` declarations/grammar | Core | `ast:tests/ast_basic.hop`, `ast:tests/ast_type_alias.hop`, `ast:tests/ast_fn_type.hop`, `ast:tests/ast_anon_aggregate_types.hop` |
| `DECL-HOLE-*` and reserved naming | Core | `check:tests/hole_local_ok.hop`, `check:tests/hole_param_codegen_ok.hop`, `check:tests/hole_*_bad.hop`, `check:tests/bad_reserved_name_*.hop` |
| `DECL-FN-*` overloads | Core | `check:tests/typefunc_overload_ok.hop`, `check:tests/typefunc_arity_overload_ok.hop`, `check:tests/typefunc_bad_no_matching_overload.hop`, `check:tests/typefunc_bad_ambiguous_call.hop` |
| Struct embedding / enum member namespace | Core | `check:tests/struct_composition_ok.hop`, `check:tests/ptr_embedded_upcast_ok.hop`, `check:tests/enum_member_namespace_ok.hop`, `check:tests/enum_member_unqualified_bad.hop` |
| Type aliases and directionality | Core | `check:tests/type_alias_direction_bad.hop`, `integration.runtime.typefunc_alias_*.compile_and_run` |
| Optional typing and narrowing | Core | `check:tests/feature_optional_ok.hop`, `check:tests/feature_optional_no_import.hop`, `check:tests/hep3_null_unwrap.hop`, `check:tests/hep3_flow_narrow_ok.hop` |
| Function types | Core | `ast:tests/ast_fn_type.hop`, `check:tests/fntype_ok.hop`, `integration.runtime.fntype_ok.compile_and_run` |
| Anonymous aggregate types/literals | Core | `ast:tests/ast_anon_aggregate_types.hop`, `check:tests/anon_struct_infer_no_context_ok.hop`, `integration.examples.anonymous_aggregates.compile_only` |
| Expression typing and comparisons | Core | `check:tests/bad_type_mismatch.hop`, `integration.examples.comparison.compile_only`, `integration.runtime.len_ptr_ref_ok.compile_only` |
| Selector-call sugar and precedence | Core | `check:tests/typefunc_selector_autoref_ok.hop`, `check:tests/typefunc_selector_autoref_temp_bad.hop`, `check:tests/typefunc_field_precedence_bad.hop` |
| Statement typing (`if/for/switch/assert/defer`) | Core | `check:tests/switch_ok.hop`, `check:tests/switch_bad_*.hop`, `check:tests/assert_ok.hop`, `integration.examples.defer.compile_only`, `integration.examples.control_flow.compile_only` |
| Context declaration/call overlays | Core | `checkpkg:tests/context_ok.hop`, `checkpkg:tests/context_temp_mem_ok.hop`, `check:tests/context_missing_field_bad.hop`, `integration.examples.context.compile_only` |
| Built-ins `len/cstr/panic/sizeof/print/concat/free` | Core | `check:tests/print_ok.hop`, `check:tests/panic_ok.hop`, `checkpkg:tests/str_concat_free_ok.hop`, `integration.runtime.print_ok.compile_and_run`, `integration.runtime.str_slice_conversion_ok.compile_only` |
| Built-in `new` | Core | `checkpkg:tests/new_ok.hop`, `checkpkg:tests/new_selector_ok.hop`, `checkpkg:tests/new_contextual_count_ok.hop`, `checkpkg:tests/new_vss_init_ok.hop`, plus `new_bad_*` failures |
| Variable-size structs | Core | `check:tests/vss_ok.hop`, `check:tests/vss_nested_ok.hop`, `integration.codegen.vss*.genpkg_c_compile` |
| Imports/path/alias/named symbols | Core | `checkpkg:tests/import_*`, `checkpkg:tests/enum_member_import_*`, `checkpkg:tests/platform_import_*` |
| Exports/public API closure | Core | `checkpkg:tests/pkg_ok/app`, `checkpkg:tests/var_infer_transitive_type_ok/c`, `checkpkg:tests/typefunc_import_*` |
| Entrypoint validation | Core | `integration.runtime.main_i32_bad.hop_run`, `integration.runtime.run_exit7.*` |

## 2. Reference-hop Coverage

| Rule family | Profile | Primary evidence tests |
|---|---|---|
| `REF-IMPL-002` optional unwrap lowering | Ref | `integration.codegen.new_ok.unwrap_lowering`, `integration.runtime.new_nonoptional_panic.compile_and_run` |
| `REF-IMPL-003` `new` destination-sensitive unwrap | Ref | `integration.codegen.new_optional.no_unwrap_lowering`, `integration.codegen.new_selector_optional.no_unwrap_lowering` |
| `REF-IMPL-001` switch lowering form | Ref | indirect via codegen suites; no dedicated syntax-shape golden in current manifest |
| `REF-IMPL-004` runtime bounds-check emission limits | Ref | negative slice checks currently exercised as type/static failures (`slice_bad_*`) |
| `PKG-IMPORT-009` feature import warning behavior | Ref | `check:tests/feature_optional_no_import.hop` (acceptance), warning text not normatively asserted |

## 3. Current Gaps (Recommended Additional Tests)

The following Stable-rule areas are only partially covered by explicit tests and should be expanded:

1. Function-type parameter-group edge cases (`fn(a, b T)` mixed with plain type params).
2. Anonymous union-by-value mutation/read semantics beyond compile-only acceptance.
3. Expression-switch subject-evaluation semantics (single-evaluation behavior with side-effectful subjects).
4. More explicit tests for `main` + explicit `context` clause rejection.
5. Import normalization edge cases (`.` / `..` path collapsing and root escape errors).
