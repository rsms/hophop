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
test=0    # run tests after successful build (implies 'clean')
cc=clang  # compiler to use (also used for linking)
release=0 # compatibility alias for debug=0
analyze=0 # run clang analyzer on source code and exit
toolchain=
jobs=     # explicit number of -j or --jobs to use for running tests (defaults to CPU count)
c_backend=1 # build C backend support into slc
wasm_backend=1 # build Wasm backend support into slc
[ "$*" = "--help" ] && { echo "Usage: $0 [var[=value] ...]"; exit 0; };
for a in "$@"; do
    case "$a" in
    *=*)
        k=${a%%=*}
        v=${a#*=}
        declare -p "$k" >/dev/null 2>&1 || _err "unknown option '$k'"
        declare "$k=$v"
        ;;
    *)
        k=$a
        declare -p "$k" >/dev/null 2>&1 || _err "unknown option '$k'"
        declare "$k=1"
        ;;
    esac
done
[ "${release:-}" = 1 ] && debug=0
asan=${asan:-$debug} # enable by default in debug builds
mode=debug; [ $debug = 0 ] && mode=release
[ $test = 0 ] || clean=1  # 'test' implies 'clean'
build_dir=_build/$sys-$arch-$mode
diag_json=src/diagnostics.jsonl
diag_tool=tools/gen_diagnostics.py
diag_enum_out=src/diagnostics_enum.inc
diag_c_out=src/diagnostics_data.c
diag_outputs=( "$diag_enum_out" "$diag_c_out" )
git_index_dep=.git/index
if [ -f .git ]; then
    git_index_dep=$(git rev-parse --git-path index 2>/dev/null || echo .git/index)
fi

cli_sources=(
    src/evaluator.c
    src/slc_support.c
    src/slc_pkg.c
    src/slc_mir.c
    src/slc_codegen.c
    src/slc_main.c
    src/platform_cli-eval.c
)
if [ $c_backend = 1 ]; then
    lib_sources=(
        $(find src -maxdepth 2 -name '*.c' \
            -and -not -name 'evaluator.c' \
            -and -not -name 'slc_support.c' \
            -and -not -name 'slc_pkg.c' \
            -and -not -name 'slc_mir.c' \
            -and -not -name 'slc_codegen.c' \
            -and -not -name 'slc_main.c' \
            -and -not -name 'platform_cli-eval.c' | sort -V)
    )
else
    lib_sources=(
        $(find src -maxdepth 2 -name '*.c' \
            -and -not -path 'src/codegen_c/*' \
            -and -not -name 'evaluator.c' \
            -and -not -name 'slc_support.c' \
            -and -not -name 'slc_pkg.c' \
            -and -not -name 'slc_mir.c' \
            -and -not -name 'slc_codegen.c' \
            -and -not -name 'slc_main.c' \
            -and -not -name 'platform_cli-eval.c' | sort -V)
    )
fi
case " ${lib_sources[*]} " in
*" $diag_c_out "*) ;;
*) lib_sources+=( "$diag_c_out" ) ;;
esac
libsl_sources=()
for srcfile in "${lib_sources[@]}"; do
    libsl_sources+=( "$srcfile" )
done
lib_headers=( $(find src -maxdepth 2 -name '*.h' | sort -V) )
builtin_sl_sources=( $(find lib/builtin -maxdepth 1 -name '*.sl' | sort -V) )
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
    -Wc2x-compat \
    -Wc2x-extensions \
    -Wno-unused \
    -Wno-unused-parameter \
    -Wno-bitwise-op-parentheses \
    -Wno-shift-op-parentheses \
    -Werror=c2x-compat \
    -Werror=c2x-extensions \
    -Werror=format \
    -Werror=incompatible-pointer-types \
    -Werror=return-type \
    -Werror=switch \
    -Werror=enum-conversion \
    -DSL_WITH_C_BACKEND=$c_backend \
    -DSL_WITH_WASM_BACKEND=$wasm_backend \
    $(_if_release -O2 -DNDEBUG -flto=thin) \
)
l_flags=()

if [ $asan = 1 -o $ubsan = 1 ]; then
    c_flags+=( -fno-sanitize-recover=all )
    [ $debug = 1 ] || c_flags+=( -fsanitize-trap=all )
    if [ $asan = 1 ]; then
        x_flags+=( -fsanitize=address )
        if [ $debug = 1 ]; then
            c_flags+=( -fno-omit-frame-pointer -fno-optimize-sibling-calls )
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
c_backend       = $c_backend
wasm_backend    = $wasm_backend
cli_sources     = ${cli_sources[@]:-}
lib_sources     = ${lib_sources[@]:-}
libsl_sources   = ${libsl_sources[@]:-}
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

    echo "Formatting ${#format_files[@]} files"
    clang-format --Werror --style=file:clang-format.yaml -i "${format_files[@]}"
fi

####################################################################################################
# analyze
if [ $analyze != 0 ]; then
    # See `clang --help | grep -A1 analyzer-output`
    rm -rf "$build_dir/analyze"
    mkdir -p "$build_dir/analyze"
    a_flags=( "${x_flags[@]}" "${c_flags[@]}" )
    a_flags=( "${a_flags[@]/-fsani*/}" )
    a_flags=( "${a_flags[@]/-fcolor-diagnostics*/}" )
    a_flags+=( -isystem lib )
    a_flags+=( --analyze -Xanalyzer -analyzer-output=sarif )
    a_files=( "${cli_sources[@]}" "${lib_sources[@]}" )
    echo "Analyzing ${#a_files[@]} files"
    for srcfile in "${a_files[@]}"; do
        report=$build_dir/analyze/$srcfile.json
        mkdir -p "$(dirname "$report")"
        clang "${a_flags[@]}" -o "$report" $srcfile > /dev/null 2>&1 &
    done
    wait
    total_issues=0
    lines=()
    while IFS= read -r -d '' f; do
        n=$(jq '[.runs[].results[]?] | length' "$f")
        if [ "$n" -gt 0 ]; then
            total_issues=$((total_issues + n))
            rel=${f#_build/macos-aarch64-debug/analyze/}
            src=${rel%.json}
            if [ "$n" -eq 1 ]; then
                lines+=("$(printf '%s: %d issue: %s' "$src" "$n" "$f")")
            else
                lines+=("$(printf '%s: %d issues: %s' "$src" "$n" "$f")")
            fi
        fi
    done < <(find _build/macos-aarch64-debug/analyze -type f -name '*.json' -print0)
    if [ ${#lines[@]} -gt 0 ]; then
        printf '%s\n' "${lines[@]}" | sort
    fi
    if [ "$total_issues" -eq 1 ]; then
        echo "Total: $total_issues issue"
    else
        echo "Total: $total_issues issues"
    fi
    if [ $total_issues -gt 0 ]; then
        echo "Tip: Run the following to analyze a specific file:"
        echo "    clang ${a_flags[*]} -o - <srcfile> 2>/dev/null"
        exit 1
    else
        exit 0
    fi
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
    command = mkdir -p \`dirname \$out\` && cp \$in \$out
    description = copy \$out

rule builtinabigen
    command = mkdir -p \`dirname \$out\` && python3 tools/gen_builtin_abi.py --builtin-dir lib/builtin --platform lib/platform/platform.sl --header lib/builtin/builtin.h && clang-format --Werror --style=file:clang-format.yaml -i lib/builtin/builtin.h && touch \$out
    description = generate \$out

rule diaggen
    command = python3 $diag_tool --json $diag_json --enum-out $diag_enum_out --c-out $diag_c_out && clang-format --Werror --style=file:clang-format.yaml -i $diag_enum_out $diag_c_out
    description = generate diagnostics

_END

objfiles=()
for srcfile in "${cli_sources[@]}" "${lib_sources[@]}"; do
    objfile="\$objdir/${srcfile//\//.}.o"
    objfiles+=( "$objfile" )
    echo "build $objfile: cc $srcfile | ${diag_outputs[*]}" >> $NF
    if [ $srcfile = "src/slc.c" ]; then
        SL_SOURCE_HASH=$(git rev-parse --short=20 HEAD 2>/dev/null || echo src)
        echo "  flags = -DSL_SOURCE_HASH=\\\"$SL_SOURCE_HASH\\\"" >> $NF
    elif [ $srcfile = "src/platform_cli-eval.c" ]; then
        echo "  flags = -isystem lib" >> $NF
    fi
done

git_index=$(git rev-parse --git-dir || true)
[ -z "$git_index" ] || git_index=$git_index/index

cat << _END >> $NF
build ${diag_outputs[*]}: diaggen $diag_json $diag_tool
build \$builddir/libsl.h: amalgamate ${lib_headers[@]} ${libsl_sources[@]} | tools/amalgamate.sh tools/amalgamate.py ${git_index} ${diag_outputs[*]}
build \$builddir/lib/builtin/builtin_abi.stamp: builtinabigen ${builtin_sl_sources[@]} lib/platform/platform.sl tools/gen_builtin_abi.py
build \$builddir/lib/builtin/builtin.h: copy lib/builtin/builtin.h | \$builddir/lib/builtin/builtin_abi.stamp
build \$builddir/lib/builtin/builtin.c: copy lib/builtin/builtin.c
build \$builddir/lib/platform/cli-libc/platform.c: copy lib/platform/cli-libc/platform.c
build \$builddir/slc: link ${objfiles[*]}

default \$builddir/libsl.h \$builddir/lib/builtin/builtin.h \$builddir/lib/builtin/builtin.c \$builddir/lib/platform/cli-libc/platform.c \$builddir/slc
_END

if [ ! -f build.ninja ]; then
    mv $NF build.ninja
elif git diff --no-index --minimal build.ninja $NF > build.ninja.diff; then
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
test_args=( run --build-dir "$build_dir" --cc "$cc" )
[ -z "$jobs" ] || test_args+=( --jobs=$jobs )
[ $c_backend = 1 ] || test_args+=( --eval-only )
echo "python3 tools/test.py" "${test_args[@]}"
exec python3 tools/test.py "${test_args[@]}"
