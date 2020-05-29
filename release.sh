#!/bin/bash
set -e
set -x
if [[ -z "$BUILD_THREADS" ]] ; then
    BUILD_THREADS=1
fi
docker build -t z3 --build-arg BUILD_THREADS="$BUILD_THREADS" .
docker container create --name=z3c z3
mkdir z3
docker cp z3c:/home/test/z3/build/z3 z3/
docker container rm z3c
tagname=$(git rev-parse --abbrev-ref HEAD | sed 's!^_!!')
platform=$(uname -m)
git tag "$tagname"
git push origin "$tagname"
tar czf $tagname-$platform.tar.gz z3/*
