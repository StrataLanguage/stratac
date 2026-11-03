#pragma once

/* Initialises the LLVM x86 and AArch64 targets once per process; later calls do nothing. */
void EnsureTargetsInitialized(void);
