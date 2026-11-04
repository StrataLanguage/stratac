#!/bin/sh
# Builds the Strata REPL: compiles repl.strata and links it against libstrata.
set -e

STRATA_BUILD=../../build
STRATA_INCLUDE=../../include
LLVM_LIB=${LLVM_LIB:-/opt/homebrew/opt/llvm/lib/libLLVM.dylib}

$STRATA_BUILD/bin/stratac repl.strata -o repl.o
cc -I$STRATA_INCLUDE repl.o shim.c $STRATA_BUILD/libstrata.a $LLVM_LIB -o repl
