#!/bin/bash

rm -f *.wasm *.js *.bc

emmake make -j5 \
  SUBTARGET=cps1 SOURCES=src/mame/drivers/cps1.cpp VERBOSE=1 \
  USE_QTDEBUG=0 DONT_USE_NETWORK=1 NO_USE_MIDI=1 NO_USE_PORTAUDIO=1 \
  WEBASSEMBLY=1 TESTS=0 BENCHMARKS=0 PROFILER=0 PROFILE=0 \
  SYMBOLS=1 OPTIMIZE=3 FASTDEBUG=1

cp mamecps1* ../emushare/public/
