#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")"
_err() { echo "$0: $@" >&2; exit 1; }
_checksum() { sha256sum "$1" | cut -d' ' -f1; }
_v() { [ $verbose != 1 ] || echo "$@" >&2; "$@"; }
_if_debug() { [ $debug != 1 ] || echo "$@"; }
_if_release() { [ $debug != 0 ] || echo "$@"; }

native_arch=$(uname -m); native_arch=${native_arch/arm64/aarch64}
native_sys=$(uname -s); native_sys=${native_sys/Darwin/macos}
arch=$native_arch
sys=$native_sys
verbose=0 # print details on stderr
debug=1   # build debug products
compdb=1  # write compile_commands.json after configuring build
config=0  # just config, don't build
asan=     # enable address sanitizer (default: value of debug)
ubsan=1   # enable undefined-behavior sanitizer
format=   # run clang-format on the source (default: on if clang-format is found and debug=1)
test=0    # run tests after successful build
cc=clang  # compiler to use (also used for linking)
[ "$*" = "--help" ] && { echo "Usage: $0 [var[=value] ...]"; exit 0; };
for a in "$@"; do
    case "$a" in
    *=*) declare ${a%%=*}=${a#*=} ;;
    *)   declare $a=1 ;;
    esac
done
[ "${release:-}" = 1 ] && debug=0
asan=${asan:-$debug} # enable by default in debug builds
mode=debug; [ $debug = 0 ] && mode=release
build_dir=_build/$sys-$arch-$mode
cli_sources=( src/slc.c )
lib_sources=( $(find src -maxdepth 2 -name '*.c' -and -not -name 'slc.c' | sort) )
lib_headers=( $(find src -maxdepth 2 -name '*.h' | sort) )
cli_output=slc
lib_output=libsl.h
toolchain=${toolchain:-/opt/homebrew/opt/llvm}
[ -z "$toolchain" -a -x "$toolchain/bin/clang" ] || export PATH=$toolchain/bin:$PATH
format=${format:-$([ $debug = 1 -a -n "$(command -v clang-format)" ] && echo 1 || echo 0 )}
x_flags=(
    -g \
    $([ -t 2 ] && echo -fcolor-diagnostics || true) \
)
c_flags=(
    -std=c11 \
    -Wall \
    -Wextra \
    -Wno-unused \
    -Wno-unused-parameter \
    -Wno-bitwise-op-parentheses \
    -Wno-shift-op-parentheses \
    -Werror=format \
    -Werror=incompatible-pointer-types \
    -Werror=return-type \
    -Werror=switch \
    -Werror=enum-conversion \
    $(_if_release -O2 -DNDEBUG -flto=thin) \
)
l_flags=()

if [ $asan = 1 -o $ubsan = 1 ]; then
    c_flags+=( -fno-sanitize-recover=all )
    [ $debug = 1 ] || c_flags+=( -fsanitize-trap=all )
    if [ $asan = 1 ]; then
        x_flags+=( -fsanitize=address )
        if [ $debug = 1 ]; then
            c_flags+=( -g -fno-omit-frame-pointer -fno-optimize-sibling-calls )
        else
            l_flags+=( -fsanitize-minimal-runtime )
        fi
    fi
    if [ $ubsan = 1 -a $debug = 1 ]; then
        x_flags+=( -fsanitize=undefined,unsigned-integer-overflow )
    elif [ $ubsan = 1 ]; then
        x_flags+=( -fsanitize=signed-integer-overflow,unsigned-integer-overflow,alignment,null )
    fi
fi

if [ $verbose = 1 ]; then
cat <<- _END >&2
mode, arch, sys = $mode, $arch, $sys
toolchain, cc   = $toolchain, $cc
build_dir       = $build_dir
cli_sources     = ${cli_sources[@]:-}
lib_sources     = ${lib_sources[@]:-}
c_flags = $(printf "\n    %s" "${x_flags[@]:-}" "${c_flags[@]:-}")
l_flags = $(printf "\n    %s" "${x_flags[@]:-}" "${l_flags[@]:-}")
_END
fi

####################################################################################################
# format
[ $format = 0 ] || clang-format --Werror --style=file:clang-format.yaml -i src/*.*

####################################################################################################
# configure

mkdir -p "$build_dir/obj"
cd "$build_dir/obj"

NF=build.ninja.new
cat << _END > $NF
ninja_required_version = 1.3

builddir = $build_dir
objdir   = \$builddir/obj
c_flags  = ${x_flags[@]:-} ${c_flags[@]:-}
l_flags  = ${x_flags[@]:-} ${l_flags[@]:-}

rule link
    command = $cc \$l_flags \$flags -o \$out \$in
    description = link \$out

rule cc
    command = $cc -MMD -MF \$out.d \$c_flags \$flags -c \$in -o \$out
    depfile = \$out.d
    description = compile \$in

rule amalgamate
    command = bash amalgamate.sh $(_if_debug --debug) \$out \$in
    description = generate \$out

_END

objfiles=()
for srcfile in "${cli_sources[@]}" "${lib_sources[@]}"; do
    objfile="\$objdir/${srcfile//\//.}.o"
    objfiles+=( "$objfile" )
    echo "build $objfile: cc $srcfile" >> $NF
done

cat << _END >> $NF
build \$builddir/libsl.h: amalgamate ${lib_headers[@]} ${lib_sources[@]} | amalgamate.sh amalgamate.py .git/index
build \$builddir/slc: link ${objfiles[*]}

default \$builddir/libsl.h \$builddir/slc
_END

[[ ! -e build.ninja || "$(_checksum $NF)" != "$(_checksum build.ninja)" ]] &&
    mv $NF build.ninja && echo "build.ninja updated" || rm $NF

cd ../../..

ninja_args=( -f "$build_dir/obj/build.ninja" )
[ $verbose = 1 ] && ninja_args+=( -v )

if [ $compdb = 1 ]; then
    _v ninja "${ninja_args[@]}" -t compdb > compile_commands.json
    sed "s@ Compiler: clang@ Compiler: $(command -v clang)@" clangd.yaml > .clangd
fi

####################################################################################################
# build

[ $config = 0 ] || exit 0
_v ninja "${ninja_args[@]}"

####################################################################################################
# test

[ $test = 1 ] || exit 0
echo "running tests"
test_tmpdir=$(mktemp -d "${TMPDIR:-/tmp}/slang-tests.XXXXXX")
trap "rm -rf $test_tmpdir" EXIT

actual_codegen_app_header="$test_tmpdir/app_codegen.h"
actual_codegen_app_obj="$test_tmpdir/app_codegen.o"
actual_codegen_ptr_header="$test_tmpdir/ptr_codegen.h"
actual_codegen_ptr_obj="$test_tmpdir/ptr_codegen.o"
actual_phase5_codegen_header="$test_tmpdir/phase5_codegen.h"
actual_phase5_codegen_obj="$test_tmpdir/phase5_codegen.o"
actual_phase6_single_header="$test_tmpdir/phase6_single.h"
actual_phase6_single_obj="$test_tmpdir/phase6_single.o"
actual_phase6_single_import_header="$test_tmpdir/phase6_single_import.h"
actual_phase6_single_import_obj="$test_tmpdir/phase6_single_import.o"
actual_phase7_compile_exe="$test_tmpdir/phase7_run_exit7"
actual_phase7_compile_stdout="$test_tmpdir/phase7_compile.stdout"
actual_phase7_compile_stderr="$test_tmpdir/phase7_compile.stderr"
actual_phase7_run_stdout="$test_tmpdir/phase7_run.stdout"
actual_phase7_run_stderr="$test_tmpdir/phase7_run.stderr"
actual_phase8_vss_header="$test_tmpdir/phase8_vss.h"
actual_phase8_vss_obj="$test_tmpdir/phase8_vss.o"
actual_phase8_vss_exe="$test_tmpdir/phase8_vss"
actual_phase8_vss_nested_header="$test_tmpdir/phase8_vss_nested.h"
actual_phase8_vss_nested_obj="$test_tmpdir/phase8_vss_nested.o"
actual_phase8_vss_nested_exe="$test_tmpdir/phase8_vss_nested"

_run_slc() {
    local mode="$1"
    shift
    if [ "$mode" = "_" ]; then
        "$build_dir/slc" "$@"
    else
        "$build_dir/slc" "$mode" "$@"
    fi
}

_run_id() {
    local s="$1"
    s=${s//\//_}
    s=${s//./_}
    s=${s//-/_}
    echo "$s"
}

_expect_stdout_eq() {
    local mode="$1"
    local input="$2"
    local expected="$3"
    local id="$(_run_id "${mode}_${input}")"
    local out="$test_tmpdir/$id.stdout"
    local err="$test_tmpdir/$id.stderr"
    if ! _run_slc "$mode" "$input" > "$out" 2> "$err"; then
        _err "unexpected failure for slc $mode $input"
    fi
    [ ! -s "$err" ] || _err "unexpected stderr for slc $mode $input"
    diff -u "$expected" "$out"
}

_expect_ok_silent() {
    local mode="$1"
    local input="$2"
    local id="$(_run_id "${mode}_${input}")"
    local out="$test_tmpdir/$id.stdout"
    local err="$test_tmpdir/$id.stderr"
    if ! _run_slc "$mode" "$input" > "$out" 2> "$err"; then
        _err "unexpected failure for slc $mode $input"
    fi
    [ ! -s "$out" ] || _err "unexpected stdout for slc $mode $input"
    [ ! -s "$err" ] || _err "unexpected stderr for slc $mode $input"
}

_expect_fail_with_stderr() {
    local mode="$1"
    local input="$2"
    local expected_stderr="$3"
    local id="$(_run_id "${mode}_${input}")"
    local out="$test_tmpdir/$id.stdout"
    local err="$test_tmpdir/$id.stderr"
    if _run_slc "$mode" "$input" > "$out" 2> "$err"; then
        _err "expected failure for slc $mode $input"
    fi
    [ ! -s "$out" ] || _err "unexpected stdout for slc $mode $input"
    diff -u "$expected_stderr" "$err"
}

_expect_fail_no_stdout() {
    local mode="$1"
    local input="$2"
    local id="$(_run_id "${mode}_${input}")"
    local out="$test_tmpdir/$id.stdout"
    local err="$test_tmpdir/$id.stderr"
    if _run_slc "$mode" "$input" > "$out" 2> "$err"; then
        _err "expected failure for slc $mode $input"
    fi
    [ ! -s "$out" ] || _err "unexpected stdout for slc $mode $input"
}

_expect_ok_with_stderr() {
    local mode="$1"
    local input="$2"
    local expected_stderr="$3"
    local id="$(_run_id "${mode}_${input}")"
    local out="$test_tmpdir/$id.stdout"
    local err="$test_tmpdir/$id.stderr"
    if ! _run_slc "$mode" "$input" > "$out" 2> "$err"; then
        _err "unexpected failure for slc $mode $input"
    fi
    [ ! -s "$out" ] || _err "unexpected stdout for slc $mode $input"
    diff -u "$expected_stderr" "$err"
}

for t in \
    "_|tests/basic.sl|tests/basic.tokens" \
    "ast|tests/ast_basic.sl|tests/ast_basic.ast" \
    "ast|tests/switch_ast.sl|tests/switch_ast.ast" \
    "ast|tests/ast_types_new.sl|tests/ast_types_new.ast" \
    "ast|tests/ast_slice_expr.sl|tests/ast_slice_expr.ast"
do
    IFS='|' read -r mode input expected <<< "$t"
    _expect_stdout_eq "$mode" "$input" "$expected"
done

for t in \
    "check|tests/order_independent.sl" \
    "check|tests/switch_ok.sl" \
    "check|tests/slice_ok.sl" \
    "check|tests/slice_fn_ok.sl" \
    "check|tests/types_mut_ok.sl" \
    "check|tests/len_ptr_ref_ok.sl" \
    "check|tests/len_null_ptr_ref_ok.sl" \
    "check|tests/new_ok.sl" \
    "check|tests/feature_optional_ok.sl" \
    "checkpkg|tests/pkg_ok/app"
do
    IFS='|' read -r mode input <<< "$t"
    _expect_ok_silent "$mode" "$input"
done

for t in \
    "_|tests/bad_string.sl|tests/bad_string.stderr" \
    "check|tests/bad_pub_block.sl|tests/bad_pub_block.stderr" \
    "ast|tests/ast_bad.sl|tests/ast_bad.stderr" \
    "ast|tests/ast_bad_old_array_type.sl|tests/ast_bad_old_array_type.stderr" \
    "ast|tests/ast_bad_old_varray_type.sl|tests/ast_bad_old_varray_type.stderr" \
    "ast|tests/ast_bad_ref_slice_type.sl|tests/ast_bad_ref_slice_type.stderr" \
    "ast|tests/ast_bad_mutref_slice_type.sl|tests/ast_bad_mutref_slice_type.stderr" \
    "check|tests/bad_unknown_symbol.sl|tests/bad_unknown_symbol.stderr" \
    "check|tests/bad_type_mismatch.sl|tests/bad_type_mismatch.stderr" \
    "check|tests/switch_bad_subject_type.sl|tests/switch_bad_subject_type.stderr" \
    "check|tests/switch_bad_condition_type.sl|tests/switch_bad_condition_type.stderr" \
    "check|tests/switch_bad_default_dup.sl|tests/switch_bad_default_dup.stderr" \
    "check|tests/slice_bad_index_oob.sl|tests/slice_bad_index_oob.stderr" \
    "check|tests/slice_bad_range_oob.sl|tests/slice_bad_range_oob.stderr" \
    "check|tests/slice_bad_range_order.sl|tests/slice_bad_range_order.stderr" \
    "check|tests/slice_bad_negative_index.sl|tests/slice_bad_negative_index.stderr" \
    "check|tests/slice_bad_negative_range.sl|tests/slice_bad_negative_range.stderr" \
    "check|tests/new_bad_allocator_readonly.sl|tests/new_bad_allocator_readonly.stderr" \
    "check|tests/new_bad_type_arg_value.sl|tests/new_bad_type_arg_value.stderr" \
    "check|tests/new_bad_arity.sl|tests/new_bad_arity.stderr" \
    "check|tests/types_mut_bad_readonly_to_mutref_assign.sl|tests/types_mut_bad_readonly_to_mutref_assign.stderr" \
    "check|tests/types_mut_bad_ref_assign_value.sl|tests/types_mut_bad_ref_assign_value.stderr" \
    "check|tests/types_mut_bad_readonly_slice_write.sl|tests/types_mut_bad_readonly_slice_write.stderr" \
    "check|tests/types_mut_bad_readonly_ref_write.sl|tests/types_mut_bad_readonly_ref_write.stderr" \
    "check|tests/bad_void_return_type.sl|tests/bad_void_return_type.stderr" \
    "checkpkg|tests/pkg_bad_symbol/app|tests/pkg_bad_symbol.stderr" \
    "checkpkg|tests/pkg_cycle/a|tests/pkg_cycle.stderr" \
    "checkpkg|tests/pub_missing_def|tests/pub_missing_def.stderr" \
    "check|tests/feature_optional_no_import.sl|tests/feature_optional_no_import.stderr"
do
    IFS='|' read -r mode input expected_stderr <<< "$t"
    _expect_fail_with_stderr "$mode" "$input" "$expected_stderr"
done

_expect_ok_with_stderr check tests/feature_unknown.sl tests/feature_unknown.stderr

"$build_dir/slc" genpkg tests/pkg_ok/app > "$actual_codegen_app_header"
rg -F "__sl_i32 app__main(void);" "$actual_codegen_app_header" > /dev/null \
    || _err "missing implicit public declaration for app main"
if rg -F "static __sl_i32 app__main(void)" "$actual_codegen_app_header" > /dev/null; then
    _err "app main should not be emitted as static"
fi
cat > "$test_tmpdir/app_codegen_test.c" << _END
#define APP_IMPL
#include "$actual_codegen_app_header"
int test_codegen_app_main(void) { return (int)app__main(); }
_END
"$cc" -std=c11 -Wall -Wextra -Werror -c "$test_tmpdir/app_codegen_test.c" -o "$actual_codegen_app_obj"

"$build_dir/slc" genpkg:c tests/codegen_ptr > "$actual_codegen_ptr_header"
cat > "$test_tmpdir/ptr_codegen_test.c" << _END
#define DEMO_IMPL
#include "$actual_codegen_ptr_header"
_END
"$cc" -std=c11 -Wall -Wextra -Werror -c "$test_tmpdir/ptr_codegen_test.c" -o "$actual_codegen_ptr_obj"

_expect_ok_silent check tests/assert_ok.sl
_expect_fail_with_stderr check tests/assert_bad_condition.sl tests/assert_bad_condition.stderr

"$build_dir/slc" genpkg:c tests/codegen_strings_assert > "$actual_phase5_codegen_header"
rg -F "typedef struct { __sl_u32 len; __sl_u8 bytes[1]; } sl_strhdr;" "$actual_phase5_codegen_header" > /dev/null \
    || _err "missing sl_strhdr prelude type in phase5 codegen output"
rg -F "SL_ASSERT_FAIL(__FILE__, __LINE__, \"assertion failed\");" "$actual_phase5_codegen_header" > /dev/null \
    || _err "missing SL_ASSERT_FAIL lowering in phase5 codegen output"
rg -F "SL_ASSERTF_FAIL(__FILE__, __LINE__, \"x=%d\", x);" "$actual_phase5_codegen_header" > /dev/null \
    || _err "missing SL_ASSERTF_FAIL lowering in phase5 codegen output"
[ "$(rg -c '^static const struct \{ __sl_u32 len; __sl_u8 bytes\[' "$actual_phase5_codegen_header")" = "1" ] \
    || _err "expected exactly one pooled string literal in phase5 codegen output"
cat > "$test_tmpdir/phase5_codegen_test.c" << _END
#define DEMO_IMPL
#include "$actual_phase5_codegen_header"
int test_codegen_phase5(void) { return (int)codegen_strings_assert__Main(7); }
_END
"$cc" -std=c11 -Wall -Wextra -Werror -c "$test_tmpdir/phase5_codegen_test.c" -o "$actual_phase5_codegen_obj"

for t in \
    "checkpkg|tests/import_default_alias/app" \
    "checkpkg|tests/import_invalid_alias_explicit/app" \
    "checkpkg|tests/single_file/main.sl"
do
    IFS='|' read -r mode input <<< "$t"
    _expect_ok_silent "$mode" "$input"
done
_expect_fail_with_stderr checkpkg tests/import_invalid_alias/app tests/import_invalid_alias.stderr
"$build_dir/slc" genpkg:c tests/single_file/main.sl > "$actual_phase6_single_header"
cat > "$test_tmpdir/phase6_single_test.c" << _END
#define SINGLE_FILE_IMPL
#include "$actual_phase6_single_header"
int test_codegen_phase6_single(void) { return (int)single_file__main(); }
_END
"$cc" -std=c11 -Wall -Wextra -Werror -c "$test_tmpdir/phase6_single_test.c" -o "$actual_phase6_single_obj"

_expect_ok_silent checkpkg tests/single_file_import/app/main.sl
"$build_dir/slc" genpkg:c tests/single_file_import/app/main.sl > "$actual_phase6_single_import_header"
cat > "$test_tmpdir/phase6_single_import_test.c" << _END
#define APP_IMPL
#include "$actual_phase6_single_import_header"
int test_codegen_phase6_single_import(void) { return (int)app__main(); }
_END
"$cc" -std=c11 -Wall -Wextra -Werror -c "$test_tmpdir/phase6_single_import_test.c" -o "$actual_phase6_single_import_obj"

if ! "$build_dir/slc" compile tests/run_exit7.sl -o "$actual_phase7_compile_exe" \
    > "$actual_phase7_compile_stdout" 2> "$actual_phase7_compile_stderr"; then
    _err "unexpected failure for slc compile tests/run_exit7.sl"
fi
[ ! -s "$actual_phase7_compile_stdout" ] || _err "unexpected stdout for slc compile"
[ ! -s "$actual_phase7_compile_stderr" ] || _err "unexpected stderr for slc compile"
[ -x "$actual_phase7_compile_exe" ] || _err "compile command did not produce executable output"

if ! "$build_dir/slc" compile tests/new_ok.sl -o "$test_tmpdir/step4_new_ok" > /dev/null 2>&1; then
    _err "unexpected failure for slc compile tests/new_ok.sl"
fi
[ -x "$test_tmpdir/step4_new_ok" ] \
    || _err "compile command did not produce executable for tests/new_ok.sl"

if ! "$build_dir/slc" compile tests/types_mut_ok.sl -o "$test_tmpdir/step5_types_mut_ok" \
    > /dev/null 2>&1; then
    _err "unexpected failure for slc compile tests/types_mut_ok.sl"
fi
[ -x "$test_tmpdir/step5_types_mut_ok" ] \
    || _err "compile command did not produce executable for tests/types_mut_ok.sl"

if ! "$build_dir/slc" compile tests/slice_ok.sl -o "$test_tmpdir/step5_slice_ok" \
    > /dev/null 2>&1; then
    _err "unexpected failure for slc compile tests/slice_ok.sl"
fi
[ -x "$test_tmpdir/step5_slice_ok" ] \
    || _err "compile command did not produce executable for tests/slice_ok.sl"

if ! "$build_dir/slc" compile tests/len_ptr_ref_ok.sl -o "$test_tmpdir/step5_len_ptr_ref_ok" \
    > /dev/null 2>&1; then
    _err "unexpected failure for slc compile tests/len_ptr_ref_ok.sl"
fi
[ -x "$test_tmpdir/step5_len_ptr_ref_ok" ] \
    || _err "compile command did not produce executable for tests/len_ptr_ref_ok.sl"

if ! "$build_dir/slc" compile tests/len_null_ptr_ref_ok.sl \
    -o "$test_tmpdir/step5_len_null_ptr_ref_ok" > /dev/null 2>&1; then
    _err "unexpected failure for slc compile tests/len_null_ptr_ref_ok.sl"
fi
[ -x "$test_tmpdir/step5_len_null_ptr_ref_ok" ] \
    || _err "compile command did not produce executable for tests/len_null_ptr_ref_ok.sl"
"$test_tmpdir/step5_len_null_ptr_ref_ok" > /dev/null 2>&1 \
    || _err "len null pointer/ref runtime behavior regressed"

if ! "$build_dir/slc" compile tests/slice_fn_ok.sl -o "$test_tmpdir/step5_slice_fn_ok" \
    > /dev/null 2>&1; then
    _err "unexpected failure for slc compile tests/slice_fn_ok.sl"
fi
[ -x "$test_tmpdir/step5_slice_fn_ok" ] \
    || _err "compile command did not produce executable for tests/slice_fn_ok.sl"

set +e
"$actual_phase7_compile_exe" > /dev/null 2>&1
phase7_status=$?
set -e
[ "$phase7_status" = "7" ] || _err "compiled executable returned $phase7_status, expected 7"

set +e
"$build_dir/slc" run tests/run_exit7.sl > "$actual_phase7_run_stdout" 2> "$actual_phase7_run_stderr"
phase7_status=$?
set -e
[ "$phase7_status" = "7" ] || _err "slc run returned $phase7_status, expected 7"
[ ! -s "$actual_phase7_run_stdout" ] || _err "unexpected stdout for slc run"
[ ! -s "$actual_phase7_run_stderr" ] || _err "unexpected stderr for slc run"

_expect_ok_silent check tests/vss_ok.sl
"$build_dir/slc" genpkg:c tests/vss_ok.sl > "$actual_phase8_vss_header"
rg -F "tests__Packet__payload" "$actual_phase8_vss_header" > /dev/null \
    || _err "missing payload accessor in phase8 codegen output"
rg -F "tests__Packet__samples" "$actual_phase8_vss_header" > /dev/null \
    || _err "missing samples accessor in phase8 codegen output"
rg -F "tests__Packet__sizeof" "$actual_phase8_vss_header" > /dev/null \
    || _err "missing vss sizeof helper in phase8 codegen output"
cat > "$test_tmpdir/phase8_vss_test.c" << _END
#define TESTS_IMPL
#include "$actual_phase8_vss_header"
int test_codegen_phase8_vss(tests__Packet* p) {
    __sl_u8* payload = tests__Packet__payload(p);
    __sl_i32* samples = tests__Packet__samples(p);
    __sl_uint n = tests__Packet__sizeof(p);
    return payload != (__sl_u8*)0 || samples != (__sl_i32*)0 || n > 0 ? 0 : 0;
}
_END
"$cc" -std=c11 -Wall -Wextra -Werror -c "$test_tmpdir/phase8_vss_test.c" -o "$actual_phase8_vss_obj"

if ! "$build_dir/slc" compile tests/vss_ok.sl -o "$actual_phase8_vss_exe" > /dev/null 2>&1; then
    _err "unexpected failure for slc compile tests/vss_ok.sl"
fi
[ -x "$actual_phase8_vss_exe" ] || _err "compile command did not produce executable for phase8/vss_ok.sl"

_expect_ok_silent check tests/vss_nested_ok.sl
"$build_dir/slc" genpkg:c tests/vss_nested_ok.sl > "$actual_phase8_vss_nested_header"
rg -F "tests__Section__entries" "$actual_phase8_vss_nested_header" > /dev/null \
    || _err "missing nested section accessor in phase8 codegen output"
rg -F "tests__Metadata__value" "$actual_phase8_vss_nested_header" > /dev/null \
    || _err "missing nested metadata accessor in phase8 codegen output"
rg -F "tests__Packet__metadata" "$actual_phase8_vss_nested_header" > /dev/null \
    || _err "missing packet metadata accessor in phase8 codegen output"
rg -F "tests__Packet__sections" "$actual_phase8_vss_nested_header" > /dev/null \
    || _err "missing packet sections accessor in phase8 codegen output"
rg -F "tests__Section__sizeof" "$actual_phase8_vss_nested_header" > /dev/null \
    || _err "missing section sizeof helper in phase8 codegen output"
rg -F "tests__Metadata__sizeof" "$actual_phase8_vss_nested_header" > /dev/null \
    || _err "missing metadata sizeof helper in phase8 codegen output"
rg -F "tests__Packet__sizeof" "$actual_phase8_vss_nested_header" > /dev/null \
    || _err "missing packet sizeof helper in phase8 codegen output"
cat > "$test_tmpdir/phase8_vss_nested_test.c" << _END
#define TESTS_IMPL
#include "$actual_phase8_vss_nested_header"
int test_codegen_phase8_vss_nested(tests__Packet* p, tests__Section* s, tests__Metadata* m) {
    tests__Metadata* metadata = tests__Packet__metadata(p);
    tests__Section* sections = tests__Packet__sections(p);
    __sl_u8* mvalue = tests__Metadata__value(m);
    __sl_i64* sentries = tests__Section__entries(s);
    __sl_uint psize = tests__Packet__sizeof(p);
    __sl_uint ssize = tests__Section__sizeof(s);
    __sl_uint msize = tests__Metadata__sizeof(m);
    return metadata != (tests__Metadata*)0 || sections != (tests__Section*)0 ||
                   mvalue != (__sl_u8*)0 || sentries != (__sl_i64*)0 || psize > 0 || ssize > 0 ||
                   msize > 0
               ? 0
               : 0;
}
_END
"$cc" -std=c11 -Wall -Wextra -Werror -c "$test_tmpdir/phase8_vss_nested_test.c" -o "$actual_phase8_vss_nested_obj"

if ! "$build_dir/slc" compile tests/vss_nested_ok.sl -o "$actual_phase8_vss_nested_exe" > /dev/null 2>&1; then
    _err "unexpected failure for slc compile tests/vss_nested_ok.sl"
fi
[ -x "$actual_phase8_vss_nested_exe" ] || _err "compile command did not produce executable for phase8/vss_nested_ok.sl"

for input in \
    tests/vss_bad_assign.sl \
    tests/vss_bad_var_value.sl \
    tests/vss_bad_sizeof_type.sl
do
    _expect_fail_no_stdout check "$input"
done

for example_path in examples/*.sl; do
    example_name=$(basename "$example_path" .sl)
    example_output="$test_tmpdir/example-$example_name"
    if ! "$build_dir/slc" compile "$example_path" -o "$example_output" > /dev/null 2>&1; then
        _err "unexpected failure for slc compile $example_path"
    fi
    [ -x "$example_output" ] || _err "compile command did not produce executable for $example_path"
done

# freestanding build of libsl.h
cp "$build_dir/libsl.h" "$test_tmpdir/libsl.h"
cat <<_END > "$test_tmpdir/libsl.c"
#define SL_IMPLEMENTATION
#include "libsl.h"
_END

"$cc" \
    -std=c11 \
    -ffreestanding \
    -nostdlib \
    -fno-builtin \
    -Wall \
    -Wextra \
    -Werror \
    -c "$test_tmpdir/libsl.c" \
    -o "$test_tmpdir/libsl.freestanding.o"

"$cc" \
    -target wasm32-unknown-unknown \
    -std=c11 \
    -ffreestanding \
    -nostdlib \
    -fno-builtin \
    -Wall \
    -Wextra \
    -Werror \
    -c "$test_tmpdir/libsl.c" \
    -o "$test_tmpdir/libsl.wasm"

cat > "$test_tmpdir/arena_grow_test.c" << _END
#include <stdint.h>
#include <stdlib.h>

#define SL_IMPLEMENTATION
#include "libsl.h"

typedef struct {
    uint32_t allocCount;
    uint32_t freeCount;
} ArenaStats;

static void* ArenaGrow(void* ctx, uint32_t minSize, uint32_t* outSize) {
    ArenaStats* stats = (ArenaStats*)ctx;
    uint32_t    size = minSize < 128u ? 128u : minSize;
    void*       p = malloc((size_t)size);
    if (p == NULL) {
        *outSize = 0;
        return NULL;
    }
    stats->allocCount++;
    *outSize = size;
    return p;
}

static void ArenaFree(void* ctx, void* block, uint32_t blockSize) {
    ArenaStats* stats = (ArenaStats*)ctx;
    (void)blockSize;
    free(block);
    stats->freeCount++;
}

int main(void) {
    uint8_t    storage[32];
    ArenaStats stats = { 0 };
    SLArena    arena;
    void*      p0;
    void*      p1;
    void*      p2;

    SLArenaInitEx(&arena, storage, (uint32_t)sizeof(storage), &stats, ArenaGrow, ArenaFree);

    p0 = SLArenaAlloc(&arena, 16u, 8u);
    p1 = SLArenaAlloc(&arena, 128u, 8u);
    if (p0 == NULL || p1 == NULL || stats.allocCount == 0) {
        return 1;
    }

    SLArenaReset(&arena);
    p2 = SLArenaAlloc(&arena, 64u, 8u);
    if (p2 == NULL) {
        return 2;
    }

    SLArenaDispose(&arena);
    if (stats.freeCount != stats.allocCount) {
        return 3;
    }
    return 0;
}
_END
"$cc" -std=c11 -Wall -Wextra -Werror "$test_tmpdir/arena_grow_test.c" -o "$test_tmpdir/arena_grow_test"
"$test_tmpdir/arena_grow_test"

echo "tests passed"
