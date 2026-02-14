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
lib_sources=( $(find src -name '*.c' -and -not -path src/slc.c ) )
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
build \$builddir/libsl.h: amalgamate src/libsl.h ${lib_sources[@]} | amalgamate.sh amalgamate.py .git/index
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
actual_freestanding_obj="$test_tmpdir/libsl.freestanding.o"

"$build_dir/slc" tests/phase0/basic.sl > "$actual_tokens"
diff -u tests/phase0/basic.tokens "$actual_tokens"

if "$build_dir/slc" tests/phase0/bad_string.sl > "$actual_stdout" 2> "$actual_stderr"; then
    _err "expected failure for tests/phase0/bad_string.sl"
fi
[ ! -s "$actual_stdout" ] || _err "unexpected stdout for tests/phase0/bad_string.sl"
diff -u tests/phase0/bad_string.stderr "$actual_stderr"

"$build_dir/slc" ast tests/phase1/ast_basic.sl > "$actual_ast"
diff -u tests/phase1/ast_basic.ast "$actual_ast"

if "$build_dir/slc" ast tests/phase1/ast_bad.sl > "$actual_ast_bad_stdout" 2> "$actual_ast_bad_stderr"; then
    _err "expected failure for tests/phase1/ast_bad.sl"
fi
[ ! -s "$actual_ast_bad_stdout" ] || _err "unexpected stdout for tests/phase1/ast_bad.sl"
diff -u tests/phase1/ast_bad.stderr "$actual_ast_bad_stderr"

if ! "$build_dir/slc" check tests/phase2/order_independent.sl > "$actual_check_ok_stdout" 2> "$actual_check_ok_stderr"; then
    _err "unexpected failure for tests/phase2/order_independent.sl"
fi
[ ! -s "$actual_check_ok_stdout" ] || _err "unexpected stdout for tests/phase2/order_independent.sl"
[ ! -s "$actual_check_ok_stderr" ] || _err "unexpected stderr for tests/phase2/order_independent.sl"

if "$build_dir/slc" check tests/phase2/bad_unknown_symbol.sl > "$actual_check_unknown_stdout" 2> "$actual_check_unknown_stderr"; then
    _err "expected failure for tests/phase2/bad_unknown_symbol.sl"
fi
[ ! -s "$actual_check_unknown_stdout" ] || _err "unexpected stdout for tests/phase2/bad_unknown_symbol.sl"
diff -u tests/phase2/bad_unknown_symbol.stderr "$actual_check_unknown_stderr"

if "$build_dir/slc" check tests/phase2/bad_type_mismatch.sl > "$actual_check_mismatch_stdout" 2> "$actual_check_mismatch_stderr"; then
    _err "expected failure for tests/phase2/bad_type_mismatch.sl"
fi
[ ! -s "$actual_check_mismatch_stdout" ] || _err "unexpected stdout for tests/phase2/bad_type_mismatch.sl"
diff -u tests/phase2/bad_type_mismatch.stderr "$actual_check_mismatch_stderr"

"$cc" \
    -std=c11 \
    -ffreestanding \
    -fno-builtin \
    -Wall \
    -Wextra \
    -Werror \
    -DSL_IMPLEMENTATION \
    -xc \
    -c "$build_dir/libsl.h" \
    -o "$actual_freestanding_obj"

echo "tests passed"
