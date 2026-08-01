#ifndef RIBLANG_TINYCC_CONFIG_H
#define RIBLANG_TINYCC_CONFIG_H

/* Minimal static configuration for the embedded Riblang libtcc build. */
#define TCC_VERSION "0.9.28-strata"
#define CONFIG_TCC_PREDEFS 1
#define CONFIG_TCC_BACKTRACE 0
#define CONFIG_TCC_BCHECK 0
#define CONFIG_TCC_SEMLOCK 1
#define CONFIG_DWARF_VERSION 0
#define CONFIG_NEW_MACHO 1

#endif
