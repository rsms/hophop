#!/bin/bash
# usege: amalgamate.sh [--debug] <output-file> <main-header-file> <header-or-source-file>...
set -euo pipefail

args=
if [ "$1" = "--debug" ]; then
    shift
    args="--source-map --origin-comments"
fi

outfile=$1; shift
hfile=$1; shift
source_hash=$(git rev-parse --short=20 HEAD 2>/dev/null || echo src)
version_api=$(grep -F '#define SL_VERSION_API ' src/libsl.h | awk '{print $3}')
version=$(grep     -F '#define SL_VERSION '     src/libsl.h | awk '{print $3}')

index_file=$(mktemp)
trap "rm -f $index_file $outfile.tmp" EXIT

cat <<__END__ > $index_file
/* libsl version $version_api.$version <https://github.com/rsms/slang>
////////////////////////////////////////////////////////////////////////////////////////////////////
$(cat LICENSE.txt)
//////////////////////////////////////////////////////////////////////////////////////////////////*/
#define SL_SOURCE_HASH "${source_hash}"
#include "src/libsl.h"
__END__
for f in "$@"; do
    case "$f" in
        */libsl.h|libsl.h|*/libsl-impl.h|libsl-impl.h) ;;
        *.h) echo "#include \"$f\"" >> $index_file ;;
    esac
done


cat <<__END__ >> $index_file
//////////////////////////////////////////////////////////////////////////////////////////////////*/
// SL_IMPLEMENTATION
//////////////////////////////////////////////////////////////////////////////////////////////////*/
#include "src/libsl-impl.h"
__END__
for f in "$@"; do
    case "$f" in
        *.h) ;;
        *) echo "#include \"$f\"" >> $index_file ;;
    esac
done
echo "#endif /* SL_IMPLEMENTATION */" >> $index_file

# cp $index_file "$(dirname "$outfile")/amalgamate.h"

python3 amalgamate.py -r . -r src $args -o "$outfile" $index_file
