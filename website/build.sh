#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")"

test build/package-lock.json -nt build/package.json -a -f build/node_modules/.bin/mkweb || {
    cd build
    echo "Running 'npm install' in $PWD"
    npm install
    cd ..
}

if test \
    _favicon/favicon-32.png -nt favicon.ico -o \
    _favicon/favicon-128.png -nt favicon.ico
then
    echo "generate favicon.ico from" _favicon/*.png
    magick _favicon/*.png favicon.ico || true
fi

build/node_modules/.bin/mkweb -config build/mkweb.js "$@"
