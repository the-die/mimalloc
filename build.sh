#! /bin/bash

cmake -S . -B build -DCMAKE_EXPORT_COMPILE_COMMANDS=ON -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_INSTALL_PREFIX=./deploy -DMI_SECURE=ON -DMI_DEBUG_FULL=ON

cmake --build build -j 8

cmake --install build
