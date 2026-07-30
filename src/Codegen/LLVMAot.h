// Strata compiler: ahead-of-time native code emission via LLVM TargetMachine.
//
// Lowers a built LLVM module to a native object file (.o / .obj) or assembly
// (.s) in-process, using the linked LLVM-C.dll. This is the "compile scripts at
// build time / cache them to disk" path; the JIT (LLVMJit) is the runtime path.
//
// Internal header (LLVM-enabled sources only).
#pragma once

#include "LLVMModuleBuilder.h"

#include <string>

namespace strata
{

// Writes native code for `bm` to `path`.
//   assembly = true  -> assembly source (.s)
//   assembly = false -> relocatable object (.o / .obj)
// `targetTriple` overrides the host default; when empty, the host architecture
// is used (e.g. x86_64-pc-windows-msvc on a 64-bit Windows host). For AArch64
// pass "aarch64-pc-windows-msvc" (Windows) or "aarch64-unknown-linux-gnu" (Linux).
// Returns true on success; `errorMessage` is set on failure.
bool EmitNativeFile(const BuiltModule& bm, const std::string& path, bool assembly, std::string& errorMessage,
                    const std::string& targetTriple = {});

} // namespace strata
