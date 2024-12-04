#!/bin/bash
if [ ! -d "build" ]; then
  mkdir build
fi

cd build && cmake .. && make -j $(nproc) && cd ..
mpiexec --allow-run-as-root -np $1 ./build/nts $2
#mpiexec --allow-run-as-root -np 1 ./nts NTS_cora_data.cfg



