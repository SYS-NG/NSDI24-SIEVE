#!/bin/bash 
set -euo pipefail


SORUCE=$(readlink -f ${BASH_SOURCE[0]})
DIR=$(dirname ${SORUCE})

echo "Changing directory to ${DIR}/../"
cd ${DIR}/../;
echo "Created _build directory (if it didn't exist)"
mkdir _build || true 2>/dev/null;
echo "Changing into _build directory"
cd _build;
echo "Running cmake..."
cmake ..;
echo "Building with make..."
make -j;
echo "Changing back to ${DIR}"
cd ${DIR}; 
echo "Installation of libCacheSim completed."

