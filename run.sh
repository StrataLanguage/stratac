#!/bin/sh
# ==========================================================================
#  run.sh -- compile a Strata program to a native executable and run it.
#
#  Usage:
#     run.sh  <file.strata>                   standalone (file needs `int main()`)
#     run.sh  <file.strata> <host.c>          link a host driver (provides main + externs)
#
#  The host driver supplies `extern` functions and `int main`; the Strata file
#  then does not need its own `main`. Tools can be overridden with the STRATAC
#  and CLANG environment variables.
# ==========================================================================
set -e

if [ $# -lt 1 ]; then
    echo "Usage: run.sh <file.strata> [host.c]"
    echo ""
    echo "Standalone:  run.sh hello.strata            (.strata defines int main())"
    echo "With host:   run.sh engine_api.strata hosts/engine_api_host.c"
    echo "             (host provides main + extern functions)"
    exit 2
fi

SRC=$1
if [ ! -f "$SRC" ]; then
    echo "error: input file not found: $SRC"
    exit 2
fi

HOST=""
if [ $# -ge 2 ]; then
    HOST=$2
    if [ ! -f "$HOST" ]; then
        echo "error: host file not found: $HOST"
        exit 2
    fi
fi

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)

STRATAC="${STRATAC:-$SCRIPT_DIR/build/default/bin/stratac}"
CLANG="${CLANG:-clang}"

if [ ! -x "$STRATAC" ] && ! command -v "$STRATAC" >/dev/null 2>&1; then
    echo "error: stratac not found: $STRATAC  (set STRATAC to override)"
    exit 1
fi

BASE=$(basename "$SRC" | sed 's/\.[^.]*$//')
TMPDIR="${TMPDIR:-/tmp}"
OBJ="$TMPDIR/strata_${BASE}.o"
EXE="$TMPDIR/strata_${BASE}"
LOG="$TMPDIR/strata_${BASE}.log"

echo "[1/3] compile  $SRC -> $OBJ"
"$STRATAC" "$SRC" -o "$OBJ" >"$LOG" 2>&1 || {
    echo "compile failed:"
    cat "$LOG"
    rm -f "$OBJ" "$LOG"
    exit 1
}

if [ -n "$HOST" ]; then
    echo "[2/3] link     $HOST + $OBJ -> $EXE"
    "$CLANG" "$HOST" "$OBJ" -o "$EXE" >"$LOG" 2>&1 || {
        echo "link failed:"
        cat "$LOG"
        rm -f "$OBJ" "$EXE" "$LOG"
        exit 1
    }
else
    echo "[2/3] link     $OBJ -> $EXE"
    "$CLANG" "$OBJ" -o "$EXE" >"$LOG" 2>&1 || {
        echo "link failed. Pass a host driver as the second argument, e.g."
        echo "   run.sh $SRC hosts/driver.c"
        echo "or make sure $SRC defines int main."
        cat "$LOG"
        rm -f "$OBJ" "$EXE" "$LOG"
        exit 1
    }
fi

echo "[3/3] run      $EXE"
echo
"$EXE"
RC=$?
echo
rm -f "$OBJ" "$EXE" "$LOG"
echo "exit code: $RC"
exit $RC
