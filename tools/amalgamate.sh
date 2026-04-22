#!/bin/bash
# usege: amalgamate.sh [--debug] <output-file> <header-or-source-file>...
set -euo pipefail

args=
if [ "$1" = "--debug" ]; then
    shift
    args="--source-map --origin-comments"
fi

outfile=$1; shift
source_hash=$(git rev-parse --short=20 HEAD 2>/dev/null || echo src)
version=$(grep -F '#define HOP_VERSION ' src/libhop.h | awk '{print $3}')

index_file=$(mktemp)
trap "rm -f $index_file $outfile.tmp" EXIT

cat <<__END__ > $index_file
/* libhop version $version <https://github.com/rsms/hophop>
////////////////////////////////////////////////////////////////////////////////////////////////////
$(cat LICENSE.txt)
//////////////////////////////////////////////////////////////////////////////////////////////////*/
#define HOP_SOURCE_HASH "${source_hash}"
#include "src/libhop.h"
__END__
for f in "$@"; do
    case "$f" in
        */libhop.h|libhop.h|*/libhop-impl.h|libhop-impl.h) ;;
        *.h) echo "#include \"$f\"" >> $index_file ;;
    esac
done


cat <<__END__ >> $index_file
/*//////////////////////////////////////////////////////////////////////////////////////////////////
// HOP_IMPLEMENTATION
//////////////////////////////////////////////////////////////////////////////////////////////////*/
#ifdef HOP_IMPLEMENTATION
#include "src/libhop-impl.h"
__END__
for f in "$@"; do
    case "$f" in
        *.h) ;;
        *) echo "#include \"$f\"" >> $index_file ;;
    esac
done
echo "#endif /* HOP_IMPLEMENTATION */" >> $index_file

# cp $index_file "$(dirname "$outfile")/amalgamate.h"

build_dir=$(dirname "$outfile")
script_dir=$(cd "$(dirname "$0")" && pwd)
python3 "$script_dir/amalgamate.py" -r . -r src -r "$build_dir" $args -o "$outfile" $index_file
