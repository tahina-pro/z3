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
commit_hash=$(git show --no-patch --format=%h)
if [[ -f tag ]] ; then
    tagname=$(cat tag)
    tagged_commit_hash=$(git show --no-patch --format=%h $tagname)
    [[ "$tagged_commit_hash" = "$commit_hash" ]]
else
    branchname=$(git rev-parse --abbrev-ref HEAD)
    tagname=$(echo $branchname | sed 's!^_!!')
    echo $tagname > tag
    git add tag
    git commit -m "release $tagname"
    git tag $tagname
    git push --set-upstream origin $branchname
    git push $tagname
fi
platform=$(uname -m)
tar czf $tagname-$platform.tar.gz z3/*
