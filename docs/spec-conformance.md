# SL Spec Conformance Matrix

This document maps rule families in [`docs/language.md`](./language.md) to existing tests.

Conventions:
- `Core` means required for Stable-rule conformance.
- `Ref` means Reference-slc profile coverage for Provisional/implementation-defined behavior.
- Exact stderr strings in current tests are treated as reference artifacts, not normative requirements.

## 1. Core Coverage Matrix

| Rule family | Profile | Primary evidence tests |
|---|---|---|
| `LEX-*` (tokens, comments, semicolon insertion baseline) | Core | `_:tests/basic.sl`, `_:tests/bad_string.sl`, `ast:tests/ast_basic.sl` |
| `SYN-*` declarations/grammar | Core | `ast:tests/ast_basic.sl`, `ast:tests/ast_fn_group.sl`, `ast:tests/ast_type_alias.sl`, `ast:tests/ast_fn_type.sl`, `ast:tests/ast_anon_aggregate_types.sl` |
| `DECL-HOLE-*` and reserved naming | Core | `check:tests/hole_local_ok.sl`, `check:tests/hole_param_codegen_ok.sl`, `check:tests/hole_*_bad.sl`, `check:tests/bad_reserved_name_*.sl` |
| `DECL-FN-*` / overload groups | Core | `check:tests/typefunc_group_ok.sl`, `check:tests/typefunc_bad_group_member.sl`, `check:tests/typefunc_bad_group_dup_member.sl`, `check:tests/typefunc_bad_group_collision.sl` |
| Struct embedding / enum member namespace | Core | `check:tests/struct_composition_ok.sl`, `check:tests/ptr_embedded_upcast_ok.sl`, `check:tests/enum_member_namespace_ok.sl`, `check:tests/enum_member_unqualified_bad.sl` |
| Type aliases and directionality | Core | `check:tests/type_alias_direction_bad.sl`, `integration.runtime.typefunc_alias_*.compile_and_run` |
| Optional typing and narrowing | Core | `check:tests/feature_optional_ok.sl`, `check:tests/feature_optional_no_import.sl`, `check:tests/slp3_null_unwrap.sl`, `check:tests/slp3_flow_narrow_ok.sl` |
| Function types | Core | `ast:tests/ast_fn_type.sl`, `check:tests/fntype_ok.sl`, `integration.runtime.fntype_ok.compile_and_run` |
| Anonymous aggregate types/literals | Core | `ast:tests/ast_anon_aggregate_types.sl`, `check:tests/anon_struct_infer_no_context_ok.sl`, `integration.examples.anonymous_aggregates.compile_only` |
| Expression typing and comparisons | Core | `check:tests/bad_type_mismatch.sl`, `integration.examples.comparison.compile_only`, `integration.runtime.len_ptr_ref_ok.compile_only` |
| Selector-call sugar and precedence | Core | `check:tests/typefunc_selector_autoref_ok.sl`, `check:tests/typefunc_selector_autoref_temp_bad.sl`, `check:tests/typefunc_field_precedence_bad.sl` |
| Statement typing (`if/for/switch/assert/defer`) | Core | `check:tests/switch_ok.sl`, `check:tests/switch_bad_*.sl`, `check:tests/assert_ok.sl`, `integration.examples.defer.compile_only`, `integration.examples.control_flow.compile_only` |
| Context declaration/call overlays | Core | `checkpkg:tests/context_ok.sl`, `checkpkg:tests/context_temp_mem_ok.sl`, `check:tests/context_missing_field_bad.sl`, `integration.examples.context.compile_only` |
| Built-ins `len/cstr/panic/sizeof/print/concat/free` | Core | `check:tests/print_ok.sl`, `check:tests/panic_ok.sl`, `checkpkg:tests/str_concat_free_ok.sl`, `integration.runtime.print_ok.compile_and_run`, `integration.runtime.str_slice_conversion_ok.compile_only` |
| Built-in `new` | Core | `checkpkg:tests/new_ok.sl`, `checkpkg:tests/new_selector_ok.sl`, `checkpkg:tests/new_contextual_count_ok.sl`, `checkpkg:tests/new_vss_init_ok.sl`, plus `new_bad_*` failures |
| Variable-size structs | Core | `check:tests/vss_ok.sl`, `check:tests/vss_nested_ok.sl`, `integration.codegen.vss*.genpkg_c_compile` |
| Imports/path/alias/named symbols | Core | `checkpkg:tests/import_*`, `checkpkg:tests/enum_member_import_*`, `checkpkg:tests/platform_import_*` |
| Exports/public API closure | Core | `checkpkg:tests/pkg_ok/app`, `checkpkg:tests/var_infer_transitive_type_ok/c`, `checkpkg:tests/typefunc_import_*` |
| Entrypoint validation | Core | `integration.runtime.main_i32_bad.slc_run`, `integration.runtime.run_exit7.*` |

## 2. Reference-slc Coverage

| Rule family | Profile | Primary evidence tests |
|---|---|---|
| `REF-IMPL-002` optional unwrap lowering | Ref | `integration.codegen.new_ok.unwrap_lowering`, `integration.runtime.new_nonoptional_panic.compile_and_run` |
| `REF-IMPL-003` `new` destination-sensitive unwrap | Ref | `integration.codegen.new_optional.no_unwrap_lowering`, `integration.codegen.new_selector_optional.no_unwrap_lowering` |
| `REF-IMPL-001` switch lowering form | Ref | indirect via codegen suites; no dedicated syntax-shape golden in current manifest |
| `REF-IMPL-004` runtime bounds-check emission limits | Ref | negative slice checks currently exercised as type/static failures (`slice_bad_*`) |
| `PKG-IMPORT-009` feature import warning behavior | Ref | `check:tests/feature_optional_no_import.sl` (acceptance), warning text not normatively asserted |

## 3. Current Gaps (Recommended Additional Tests)

The following Stable-rule areas are only partially covered by explicit tests and should be expanded:

1. Function-type parameter-group edge cases (`fn(a, b T)` mixed with plain type params).
2. Anonymous union-by-value mutation/read semantics beyond compile-only acceptance.
3. Expression-switch subject-evaluation semantics (single-evaluation behavior with side-effectful subjects).
4. More explicit tests for `main` + explicit `context` clause rejection.
5. Import normalization edge cases (`.` / `..` path collapsing and root escape errors).
