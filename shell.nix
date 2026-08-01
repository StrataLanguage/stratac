{
  pkgs ? import <nixpkgs> { },
}:

let
  llvm = pkgs.llvmPackages.llvm;
in
pkgs.mkShell {
  packages = with pkgs; [
    clang
    cmake
    libffi
    libxml2
    ninja
    llvm
    llvm.dev
    llvm.lib
    zlib
  ];

  CFLAGS = "-D_POSIX_C_SOURCE=200809L";
  LLVM_DIR = "${llvm.dev}/lib/cmake/llvm";
}
