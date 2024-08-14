#! /bin/bash

cmake -S . -B build -DCMAKE_EXPORT_COMPILE_COMMANDS=ON -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_INSTALL_PREFIX=./deploy

cmake --build build -j 4

cmake --install build
