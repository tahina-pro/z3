#!/bin/bash
set -e
set -x
if [[ -z "$BUILD_THREADS" ]] ; then
    BUILD_THREADS=1
fi
docker build -t z3 --build-arg BUILD_THREADS="$BUILD_THREADS" .
docker container create --name=z3c z3
docker cp z3c:/home/test/z3/build/z3 .
docker container remove z3c
tagname=$(git rev-parse --abbrev-ref HEAD | sed 's!^_!!')
git tag "$tagname"
git push origin "$tagname"
