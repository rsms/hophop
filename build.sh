#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")"
_err() { echo "$0: $@" >&2; exit 1; }
_checksum() { sha256sum "$1" | cut -d' ' -f1; }
_v() { [ $verbose = 0 ] || echo "$@" >&2; "$@"; }
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
clean=0   # clean up from any previous builds
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
diag_json=src/diagnostics.json
diag_tool=tools/gen_diagnostics.py
diag_enum_out=src/diagnostics_enum.inc
diag_c_out=src/diagnostics_data.c
diag_outputs=( "$diag_enum_out" "$diag_c_out" )

cli_sources=( src/slc.c )
lib_sources=( $(find src -maxdepth 2 -name '*.c' -and -not -name 'slc.c' | sort) )
case " ${lib_sources[*]} " in
*" $diag_c_out "*) ;;
*) lib_sources+=( "$diag_c_out" ) ;;
esac
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

if [ $verbose -gt 0 ]; then
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
# clean
[ $clean = 0 ] || rm -rf "$build_dir"

####################################################################################################
# format
if [ $format != 0 ]; then
    # ignore files which have "linguist-generated" or "linguist-vendored" in .gitattributes
    format_files=( $(find src lib -maxdepth 2 \( -name '*.c' -o -name '*.h' \) |
        git check-attr --stdin linguist-generated linguist-vendored |
        grep -F ': unspecified' | cut -d: -f1 | sort -u) )

    [ $verbose -lt 2 ] || echo "clang-format" "${format_files[@]}"

    clang-format --Werror --style=file:clang-format.yaml -i "${format_files[@]}"
fi

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
    command = bash tools/amalgamate.sh $(_if_debug --debug) \$out \$in
    description = generate \$out

rule copy
    command = cp \$in \$out
    description = copy \$out

rule diaggen
    command = python3 $diag_tool --json $diag_json --enum-out $diag_enum_out --c-out $diag_c_out && clang-format --Werror --style=file:clang-format.yaml -i $diag_enum_out $diag_c_out
    description = generate diagnostics

_END

objfiles=()
for srcfile in "${cli_sources[@]}" "${lib_sources[@]}"; do
    objfile="\$objdir/${srcfile//\//.}.o"
    objfiles+=( "$objfile" )
    echo "build $objfile: cc $srcfile | ${diag_outputs[*]}" >> $NF
done

cat << _END >> $NF
build ${diag_outputs[*]}: diaggen $diag_json $diag_tool
build \$builddir/libsl.h: amalgamate ${lib_headers[@]} ${lib_sources[@]} | tools/amalgamate.sh tools/amalgamate.py .git/index ${diag_outputs[*]}
build \$builddir/lib/sl-prelude.h: copy lib/sl-prelude.h
build \$builddir/lib/platform_libc.c: copy lib/platform_libc.c
build \$builddir/slc: link ${objfiles[*]}

default \$builddir/libsl.h \$builddir/lib/sl-prelude.h \$builddir/lib/platform_libc.c \$builddir/slc
_END

if git diff --no-index --minimal build.ninja $NF > build.ninja.diff; then
    rm $NF
else
    echo "build.ninja updated (diff at $build_dir/obj/build.ninja.diff)"
    mv $NF build.ninja
fi

cd ../../..

ninja_args=( -f "$build_dir/obj/build.ninja" )
[ $verbose -gt 0 ] && ninja_args+=( -v )
[ $verbose -ge 2 ] && ninja_args+=( -d explain )

if [ $compdb = 1 ]; then
    _v ninja "${ninja_args[@]}" -t compdb > _build/compile_commands.json
    sed -E \
        -e "s@\\$\{clang\}@$(command -v clang)@" \
        -e "s@\\$\{dir\}@$PWD@g" \
        tools/clangd.yaml > .clangd
fi

####################################################################################################
# build

[ $config = 0 ] || exit 0
_v ninja "${ninja_args[@]}"

####################################################################################################
# test


[ $test = 1 ] || exit 0
echo "running tests"
python3 tools/test.py run --build-dir "$build_dir" --cc "$cc"
