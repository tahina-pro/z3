#!/bin/bash

set -e
set -x

if [[ -z "$SATS_TOKEN" ]] ; then
    echo Missing environment variable: SATS_TOKEN
    exit 1
fi
if [[ -z "$Z3_THREADS" ]] ; then
    Z3_THREADS=1
fi
branchname=$(git rev-parse --abbrev-ref HEAD)
docker build -f Dockerfile.release --build-arg Z3_BRANCHNAME=$branchname --build-arg SATS_TOKEN="$SATS_TOKEN" --build-arg Z3_THREADS="$Z3_THREADS" .
