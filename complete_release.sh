#!/bin/bash

# This script requires clang and satsuki
# Alternatively, you can run `./start_release.sh`
# instead of running this script directly

set -e
set -x

# Build z3
if [[ -z "$Z3_THREADS" ]] ; then
    Z3_THREADS=1
fi
export CXX=clang++
export CC=clang
python scripts/mk_make.py
make -C build -j $Z3_THREADS

# Prepare the package
mkdir z3
cp build/z3 z3/
cp README.md z3/
cp RELEASE_NOTES z3/
cp LICENSE.txt z3/
tagname=$(echo $Z3_BRANCHNAME | sed 's!^_!!')
platform=$(uname -m)
archive=$tagname-$platform.tar.gz
tar czf $archive z3/*

# Upload the release with the tag
LANG=C.UTF-8 LC_ALL=C.UTF-8 satsuki --slug=tahina-pro/z3 --tag=$tagname --commitish=$Z3_BRANCHNAME --file=$archive
