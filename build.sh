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

build slc:     phony \$builddir/slc
build libsl.h: phony \$builddir/libsl.h
default \$builddir/libsl.h \$builddir/slc
_END

[[ ! -e build.ninja || "$(_checksum $NF)" != "$(_checksum build.ninja)" ]] &&
    mv $NF build.ninja && echo "build.ninja updated" || rm $NF

cd ../../..

ninja_args=( -f "$build_dir/obj/build.ninja" )
[ $verbose = 1 ] && ninja_args+=( -v )

if [ $compdb = 1 ]; then
    _v ninja "${ninja_args[@]}" -t compdb > compile_commands.json
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

actual_tokens="$test_tmpdir/basic.tokens"
actual_stdout="$test_tmpdir/bad_string.stdout"
actual_stderr="$test_tmpdir/bad_string.stderr"
actual_ast="$test_tmpdir/ast_basic.ast"
actual_ast_bad_stdout="$test_tmpdir/ast_bad.stdout"
actual_ast_bad_stderr="$test_tmpdir/ast_bad.stderr"
actual_check_ok_stdout="$test_tmpdir/check_ok.stdout"
actual_check_ok_stderr="$test_tmpdir/check_ok.stderr"
actual_check_unknown_stdout="$test_tmpdir/check_unknown.stdout"
actual_check_unknown_stderr="$test_tmpdir/check_unknown.stderr"
actual_check_mismatch_stdout="$test_tmpdir/check_mismatch.stdout"
actual_check_mismatch_stderr="$test_tmpdir/check_mismatch.stderr"
actual_switch_ast="$test_tmpdir/switch_ast.ast"
actual_switch_ok_stdout="$test_tmpdir/switch_ok.stdout"
actual_switch_ok_stderr="$test_tmpdir/switch_ok.stderr"
actual_switch_bad_subject_stdout="$test_tmpdir/switch_bad_subject.stdout"
actual_switch_bad_subject_stderr="$test_tmpdir/switch_bad_subject.stderr"
actual_switch_bad_condition_stdout="$test_tmpdir/switch_bad_condition.stdout"
actual_switch_bad_condition_stderr="$test_tmpdir/switch_bad_condition.stderr"
actual_switch_bad_default_stdout="$test_tmpdir/switch_bad_default.stdout"
actual_switch_bad_default_stderr="$test_tmpdir/switch_bad_default.stderr"
actual_checkpkg_ok_stdout="$test_tmpdir/checkpkg_ok.stdout"
actual_checkpkg_ok_stderr="$test_tmpdir/checkpkg_ok.stderr"
actual_checkpkg_bad_symbol_stdout="$test_tmpdir/checkpkg_bad_symbol.stdout"
actual_checkpkg_bad_symbol_stderr="$test_tmpdir/checkpkg_bad_symbol.stderr"
actual_checkpkg_cycle_stdout="$test_tmpdir/checkpkg_cycle.stdout"
actual_checkpkg_cycle_stderr="$test_tmpdir/checkpkg_cycle.stderr"
actual_checkpkg_pub_missing_stdout="$test_tmpdir/checkpkg_pub_missing.stdout"
actual_checkpkg_pub_missing_stderr="$test_tmpdir/checkpkg_pub_missing.stderr"
actual_codegen_app_header="$test_tmpdir/app_codegen.h"
actual_codegen_app_obj="$test_tmpdir/app_codegen.o"
actual_codegen_ptr_header="$test_tmpdir/ptr_codegen.h"
actual_codegen_ptr_obj="$test_tmpdir/ptr_codegen.o"
actual_phase5_assert_ok_stdout="$test_tmpdir/phase5_assert_ok.stdout"
actual_phase5_assert_ok_stderr="$test_tmpdir/phase5_assert_ok.stderr"
actual_phase5_assert_bad_stdout="$test_tmpdir/phase5_assert_bad.stdout"
actual_phase5_assert_bad_stderr="$test_tmpdir/phase5_assert_bad.stderr"
actual_phase5_codegen_header="$test_tmpdir/phase5_codegen.h"
actual_phase5_codegen_obj="$test_tmpdir/phase5_codegen.o"
actual_phase6_bad_import_stdout="$test_tmpdir/phase6_bad_import.stdout"
actual_phase6_bad_import_stderr="$test_tmpdir/phase6_bad_import.stderr"
actual_phase6_single_header="$test_tmpdir/phase6_single.h"
actual_phase6_single_obj="$test_tmpdir/phase6_single.o"
actual_phase6_single_import_header="$test_tmpdir/phase6_single_import.h"
actual_phase6_single_import_obj="$test_tmpdir/phase6_single_import.o"
actual_phase7_compile_exe="$test_tmpdir/phase7_run_exit7"
actual_phase7_compile_stdout="$test_tmpdir/phase7_compile.stdout"
actual_phase7_compile_stderr="$test_tmpdir/phase7_compile.stderr"
actual_phase7_run_stdout="$test_tmpdir/phase7_run.stdout"
actual_phase7_run_stderr="$test_tmpdir/phase7_run.stderr"
actual_phase8_vss_bad_assign_stdout="$test_tmpdir/phase8_vss_bad_assign.stdout"
actual_phase8_vss_bad_assign_stderr="$test_tmpdir/phase8_vss_bad_assign.stderr"
actual_phase8_vss_bad_var_value_stdout="$test_tmpdir/phase8_vss_bad_var_value.stdout"
actual_phase8_vss_bad_var_value_stderr="$test_tmpdir/phase8_vss_bad_var_value.stderr"
actual_phase8_vss_bad_sizeof_stdout="$test_tmpdir/phase8_vss_bad_sizeof.stdout"
actual_phase8_vss_bad_sizeof_stderr="$test_tmpdir/phase8_vss_bad_sizeof.stderr"
actual_phase8_vss_header="$test_tmpdir/phase8_vss.h"
actual_phase8_vss_obj="$test_tmpdir/phase8_vss.o"
actual_phase8_vss_exe="$test_tmpdir/phase8_vss"
actual_phase8_vss_nested_header="$test_tmpdir/phase8_vss_nested.h"
actual_phase8_vss_nested_obj="$test_tmpdir/phase8_vss_nested.o"
actual_phase8_vss_nested_exe="$test_tmpdir/phase8_vss_nested"

"$build_dir/slc" tests/basic.sl > "$actual_tokens"
diff -u tests/basic.tokens "$actual_tokens"

if "$build_dir/slc" tests/bad_string.sl > "$actual_stdout" 2> "$actual_stderr"; then
    _err "expected failure for tests/bad_string.sl"
fi
[ ! -s "$actual_stdout" ] || _err "unexpected stdout for tests/bad_string.sl"
diff -u tests/bad_string.stderr "$actual_stderr"

"$build_dir/slc" ast tests/ast_basic.sl > "$actual_ast"
diff -u tests/ast_basic.ast "$actual_ast"

if "$build_dir/slc" ast tests/ast_bad.sl > "$actual_ast_bad_stdout" 2> "$actual_ast_bad_stderr"; then
    _err "expected failure for tests/ast_bad.sl"
fi
[ ! -s "$actual_ast_bad_stdout" ] || _err "unexpected stdout for tests/ast_bad.sl"
diff -u tests/ast_bad.stderr "$actual_ast_bad_stderr"

if ! "$build_dir/slc" check tests/order_independent.sl > "$actual_check_ok_stdout" 2> "$actual_check_ok_stderr"; then
    _err "unexpected failure for tests/order_independent.sl"
fi
[ ! -s "$actual_check_ok_stdout" ] || _err "unexpected stdout for tests/order_independent.sl"
[ ! -s "$actual_check_ok_stderr" ] || _err "unexpected stderr for tests/order_independent.sl"

if "$build_dir/slc" check tests/bad_unknown_symbol.sl > "$actual_check_unknown_stdout" 2> "$actual_check_unknown_stderr"; then
    _err "expected failure for tests/bad_unknown_symbol.sl"
fi
[ ! -s "$actual_check_unknown_stdout" ] || _err "unexpected stdout for tests/bad_unknown_symbol.sl"
diff -u tests/bad_unknown_symbol.stderr "$actual_check_unknown_stderr"

if "$build_dir/slc" check tests/bad_type_mismatch.sl > "$actual_check_mismatch_stdout" 2> "$actual_check_mismatch_stderr"; then
    _err "expected failure for tests/bad_type_mismatch.sl"
fi
[ ! -s "$actual_check_mismatch_stdout" ] || _err "unexpected stdout for tests/bad_type_mismatch.sl"
diff -u tests/bad_type_mismatch.stderr "$actual_check_mismatch_stderr"

"$build_dir/slc" ast tests/switch_ast.sl > "$actual_switch_ast"
diff -u tests/switch_ast.ast "$actual_switch_ast"

if ! "$build_dir/slc" check tests/switch_ok.sl > "$actual_switch_ok_stdout" 2> "$actual_switch_ok_stderr"; then
    _err "unexpected failure for tests/switch_ok.sl"
fi
[ ! -s "$actual_switch_ok_stdout" ] || _err "unexpected stdout for tests/switch_ok.sl"
[ ! -s "$actual_switch_ok_stderr" ] || _err "unexpected stderr for tests/switch_ok.sl"

if "$build_dir/slc" check tests/switch_bad_subject_type.sl > "$actual_switch_bad_subject_stdout" 2> "$actual_switch_bad_subject_stderr"; then
    _err "expected failure for tests/switch_bad_subject_type.sl"
fi
[ ! -s "$actual_switch_bad_subject_stdout" ] || _err "unexpected stdout for tests/switch_bad_subject_type.sl"
diff -u tests/switch_bad_subject_type.stderr "$actual_switch_bad_subject_stderr"

if "$build_dir/slc" check tests/switch_bad_condition_type.sl > "$actual_switch_bad_condition_stdout" 2> "$actual_switch_bad_condition_stderr"; then
    _err "expected failure for tests/switch_bad_condition_type.sl"
fi
[ ! -s "$actual_switch_bad_condition_stdout" ] || _err "unexpected stdout for tests/switch_bad_condition_type.sl"
diff -u tests/switch_bad_condition_type.stderr "$actual_switch_bad_condition_stderr"

if "$build_dir/slc" check tests/switch_bad_default_dup.sl > "$actual_switch_bad_default_stdout" 2> "$actual_switch_bad_default_stderr"; then
    _err "expected failure for tests/switch_bad_default_dup.sl"
fi
[ ! -s "$actual_switch_bad_default_stdout" ] || _err "unexpected stdout for tests/switch_bad_default_dup.sl"
diff -u tests/switch_bad_default_dup.stderr "$actual_switch_bad_default_stderr"

if ! "$build_dir/slc" checkpkg tests/pkg_ok/app > "$actual_checkpkg_ok_stdout" 2> "$actual_checkpkg_ok_stderr"; then
    _err "unexpected failure for tests/pkg_ok/app"
fi
[ ! -s "$actual_checkpkg_ok_stdout" ] || _err "unexpected stdout for tests/pkg_ok/app"
[ ! -s "$actual_checkpkg_ok_stderr" ] || _err "unexpected stderr for tests/pkg_ok/app"

if "$build_dir/slc" checkpkg tests/pkg_bad_symbol/app > "$actual_checkpkg_bad_symbol_stdout" 2> "$actual_checkpkg_bad_symbol_stderr"; then
    _err "expected failure for tests/pkg_bad_symbol/app"
fi
[ ! -s "$actual_checkpkg_bad_symbol_stdout" ] || _err "unexpected stdout for tests/pkg_bad_symbol/app"
diff -u tests/pkg_bad_symbol.stderr "$actual_checkpkg_bad_symbol_stderr"

if "$build_dir/slc" checkpkg tests/pkg_cycle/a > "$actual_checkpkg_cycle_stdout" 2> "$actual_checkpkg_cycle_stderr"; then
    _err "expected failure for tests/pkg_cycle/a"
fi
[ ! -s "$actual_checkpkg_cycle_stdout" ] || _err "unexpected stdout for tests/pkg_cycle/a"
diff -u tests/pkg_cycle.stderr "$actual_checkpkg_cycle_stderr"

if "$build_dir/slc" checkpkg tests/pub_missing_def > "$actual_checkpkg_pub_missing_stdout" 2> "$actual_checkpkg_pub_missing_stderr"; then
    _err "expected failure for tests/pub_missing_def"
fi
[ ! -s "$actual_checkpkg_pub_missing_stdout" ] || _err "unexpected stdout for tests/pub_missing_def"
diff -u tests/pub_missing_def.stderr "$actual_checkpkg_pub_missing_stderr"

"$build_dir/slc" genpkg tests/pkg_ok/app > "$actual_codegen_app_header"
rg -F "i32 app__main(void);" "$actual_codegen_app_header" > /dev/null \
    || _err "missing implicit public declaration for app main"
if rg -F "static i32 app__main(void)" "$actual_codegen_app_header" > /dev/null; then
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

if ! "$build_dir/slc" check tests/assert_ok.sl > "$actual_phase5_assert_ok_stdout" 2> "$actual_phase5_assert_ok_stderr"; then
    _err "unexpected failure for tests/assert_ok.sl"
fi
[ ! -s "$actual_phase5_assert_ok_stdout" ] || _err "unexpected stdout for tests/assert_ok.sl"
[ ! -s "$actual_phase5_assert_ok_stderr" ] || _err "unexpected stderr for tests/assert_ok.sl"

if "$build_dir/slc" check tests/assert_bad_condition.sl > "$actual_phase5_assert_bad_stdout" 2> "$actual_phase5_assert_bad_stderr"; then
    _err "expected failure for tests/assert_bad_condition.sl"
fi
[ ! -s "$actual_phase5_assert_bad_stdout" ] || _err "unexpected stdout for tests/assert_bad_condition.sl"
diff -u tests/assert_bad_condition.stderr "$actual_phase5_assert_bad_stderr"

"$build_dir/slc" genpkg:c tests/codegen_strings_assert > "$actual_phase5_codegen_header"
rg -F "typedef struct { u32 len; u8 bytes[1]; } sl_strhdr;" "$actual_phase5_codegen_header" > /dev/null \
    || _err "missing sl_strhdr prelude type in phase5 codegen output"
rg -F "SL_ASSERT_FAIL(__FILE__, __LINE__, \"assertion failed\");" "$actual_phase5_codegen_header" > /dev/null \
    || _err "missing SL_ASSERT_FAIL lowering in phase5 codegen output"
rg -F "SL_ASSERTF_FAIL(__FILE__, __LINE__, \"x=%d\", x);" "$actual_phase5_codegen_header" > /dev/null \
    || _err "missing SL_ASSERTF_FAIL lowering in phase5 codegen output"
[ "$(rg -c '^static const struct \{ u32 len; u8 bytes\[' "$actual_phase5_codegen_header")" = "1" ] \
    || _err "expected exactly one pooled string literal in phase5 codegen output"
cat > "$test_tmpdir/phase5_codegen_test.c" << _END
#define DEMO_IMPL
#include "$actual_phase5_codegen_header"
int test_codegen_phase5(void) { return (int)codegen_strings_assert__Main(7); }
_END
"$cc" -std=c11 -Wall -Wextra -Werror -c "$test_tmpdir/phase5_codegen_test.c" -o "$actual_phase5_codegen_obj"

if ! "$build_dir/slc" checkpkg tests/import_default_alias/app > /dev/null 2>&1; then
    _err "unexpected failure for tests/import_default_alias/app"
fi

if "$build_dir/slc" checkpkg tests/import_invalid_alias/app > "$actual_phase6_bad_import_stdout" 2> "$actual_phase6_bad_import_stderr"; then
    _err "expected failure for tests/import_invalid_alias/app"
fi
[ ! -s "$actual_phase6_bad_import_stdout" ] || _err "unexpected stdout for tests/import_invalid_alias/app"
diff -u tests/import_invalid_alias.stderr "$actual_phase6_bad_import_stderr"

if ! "$build_dir/slc" checkpkg tests/import_invalid_alias_explicit/app > /dev/null 2>&1; then
    _err "unexpected failure for tests/import_invalid_alias_explicit/app"
fi

if ! "$build_dir/slc" checkpkg tests/single_file/main.sl > /dev/null 2>&1; then
    _err "unexpected failure for tests/single_file/main.sl"
fi
"$build_dir/slc" genpkg:c tests/single_file/main.sl > "$actual_phase6_single_header"
cat > "$test_tmpdir/phase6_single_test.c" << _END
#define SINGLE_FILE_IMPL
#include "$actual_phase6_single_header"
int test_codegen_phase6_single(void) { return (int)single_file__main(); }
_END
"$cc" -std=c11 -Wall -Wextra -Werror -c "$test_tmpdir/phase6_single_test.c" -o "$actual_phase6_single_obj"

if ! "$build_dir/slc" checkpkg tests/single_file_import/app/main.sl > /dev/null 2>&1; then
    _err "unexpected failure for tests/single_file_import/app/main.sl"
fi
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

if ! "$build_dir/slc" check tests/vss_ok.sl > /dev/null 2>&1; then
    _err "unexpected failure for tests/vss_ok.sl"
fi
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
    u8* payload = tests__Packet__payload(p);
    i32* samples = tests__Packet__samples(p);
    usize n = tests__Packet__sizeof(p);
    return payload != (u8*)0 || samples != (i32*)0 || n > 0 ? 0 : 0;
}
_END
"$cc" -std=c11 -Wall -Wextra -Werror -c "$test_tmpdir/phase8_vss_test.c" -o "$actual_phase8_vss_obj"

if ! "$build_dir/slc" compile tests/vss_ok.sl -o "$actual_phase8_vss_exe" > /dev/null 2>&1; then
    _err "unexpected failure for slc compile tests/vss_ok.sl"
fi
[ -x "$actual_phase8_vss_exe" ] || _err "compile command did not produce executable for phase8/vss_ok.sl"

if ! "$build_dir/slc" check tests/vss_nested_ok.sl > /dev/null 2>&1; then
    _err "unexpected failure for tests/vss_nested_ok.sl"
fi
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
    u8* mvalue = tests__Metadata__value(m);
    i64* sentries = tests__Section__entries(s);
    usize psize = tests__Packet__sizeof(p);
    usize ssize = tests__Section__sizeof(s);
    usize msize = tests__Metadata__sizeof(m);
    return metadata != (tests__Metadata*)0 || sections != (tests__Section*)0 ||
                   mvalue != (u8*)0 || sentries != (i64*)0 || psize > 0 || ssize > 0 ||
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

if "$build_dir/slc" check tests/vss_bad_assign.sl \
    > "$actual_phase8_vss_bad_assign_stdout" 2> "$actual_phase8_vss_bad_assign_stderr"; then
    _err "expected failure for tests/vss_bad_assign.sl"
fi
[ ! -s "$actual_phase8_vss_bad_assign_stdout" ] || _err "unexpected stdout for tests/vss_bad_assign.sl"

if "$build_dir/slc" check tests/vss_bad_var_value.sl \
    > "$actual_phase8_vss_bad_var_value_stdout" 2> "$actual_phase8_vss_bad_var_value_stderr"; then
    _err "expected failure for tests/vss_bad_var_value.sl"
fi
[ ! -s "$actual_phase8_vss_bad_var_value_stdout" ] || _err "unexpected stdout for tests/vss_bad_var_value.sl"

if "$build_dir/slc" check tests/vss_bad_sizeof_type.sl \
    > "$actual_phase8_vss_bad_sizeof_stdout" 2> "$actual_phase8_vss_bad_sizeof_stderr"; then
    _err "expected failure for tests/vss_bad_sizeof_type.sl"
fi
[ ! -s "$actual_phase8_vss_bad_sizeof_stdout" ] || _err "unexpected stdout for tests/vss_bad_sizeof_type.sl"

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
