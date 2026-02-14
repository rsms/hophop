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
version_api=$(grep   -F '#define SL_VERSION_API ' src/version.h | awk '{print $3}')
version=$(grep -F '#define SL_VERSION '     src/version.h | awk '{print $3}')

index_file=$(mktemp)
trap "rm -f $index_file $outfile.tmp" EXIT
sed -E \
    -e "s/\\\$\{version\}/$version_api.$version/" \
    -e '/\$\{license\}/{
    r LICENSE.txt
    d
    }' \
    "$hfile" > $index_file

for f in "$@"; do
    case "$f" in
        *.h) printf '#include "%s"\n' "$@" >> $index_file ;;
    esac
done

echo "////////////////////////////////////////////////////////////////////////////////////////////////////" >> $index_file
echo "// SL_IMPLEMENTATION" >> $index_file
echo "////////////////////////////////////////////////////////////////////////////////////////////////////" >> $index_file
echo "#ifdef SL_IMPLEMENTATION" >> $index_file

for f in "$@"; do
    case "$f" in
        *.h) ;;
        *) printf '#include "%s"\n' "$@" >> $index_file ;;
    esac
done

echo "#endif /* SL_IMPLEMENTATION */" >> $index_file

python3 amalgamate.py -r . -r src $args -o "$outfile.tmp" $index_file

sed -E -i '' \
    -e "s/#define SL_SOURCE_HASH \"src\"/#define SL_SOURCE_HASH \"${source_hash}\"/" \
    "$outfile.tmp"

mv "$outfile.tmp" "$outfile"
