#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")"

test build/package-lock.json -nt build/package.json -a -f build/node_modules/.bin/mkweb || {
    cd build
    echo "Running 'npm install' in $PWD"
    npm install
    cd ..
}

build/node_modules/.bin/mkweb -config build/mkweb.js "$@"
