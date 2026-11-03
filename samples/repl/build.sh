#!/bin/sh

../../build/bin/stratac repl.strata -o repl.o
cc repl.o shim.c -o repl
