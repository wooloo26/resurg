#!/bin/bash
# run_test.sh — Execute a single .rsg test with directive support.
#
# Usage: run_test.sh <file.rsg> <resurg> <cc> <rt_objs> <build> <runtime>
#
# Directives (parsed from leading comments):
#   // TEST: compile_error   — resurg must exit != 0
#   // TEST: runtime_error   — binary must exit != 0
#   // EXPECT-ERROR: <text>  — stderr must contain <text>
set -u

RG_FILE="$1"
RESURG="$2"
CC="$3"
RT_OBJS="$4"
BUILD="$5"
RUNTIME="$6"

# -----------------------------------------------------------------------
# Colored output helpers
# -----------------------------------------------------------------------
RED='\033[1;31m'
RESET='\033[0m'

fail() {
    printf "${RED}  FAIL  %s — %s${RESET}\n" "$RG_FILE" "$1" >&2
    if [[ -n "${2:-}" ]]; then
        printf "${RED}         cmd: %s${RESET}\n" "$2" >&2
    fi
    if [[ -n "${3:-}" ]]; then
        printf "${RED}         output: %s${RESET}\n" "$3" >&2
    fi
    exit 1
}

# Describe signal names for common crash signals
describe_exit() {
    local code=$1
    if (( code > 128 )); then
        local sig=$(( code - 128 ))
        case $sig in
            6)  echo "SIGABRT (abort)";;
            8)  echo "SIGFPE (floating point exception)";;
            9)  echo "SIGKILL (killed)";;
            11) echo "SIGSEGV (segmentation fault)";;
            *)  echo "signal $sig";;
        esac
    else
        echo "exit code $code"
    fi
}

# -----------------------------------------------------------------------
# Parse directives from leading comments
# -----------------------------------------------------------------------
TEST_MODE="normal"
EXPECT_ERROR=""

while IFS= read -r line; do
    if [[ "$line" =~ ^//\ TEST:\ (.+) ]]; then
        TEST_MODE="${BASH_REMATCH[1]}"
    elif [[ "$line" =~ ^//\ EXPECT-ERROR:\ (.+) ]]; then
        EXPECT_ERROR="${BASH_REMATCH[1]}"
    elif [[ "$line" =~ ^// ]] || [[ -z "$line" ]]; then
        continue
    else
        break
    fi
done < "$RG_FILE"

# -----------------------------------------------------------------------
# Execute based on mode
# -----------------------------------------------------------------------
case "$TEST_MODE" in
    normal)
        STDERR=$("$RESURG" "$RG_FILE" -o "$BUILD/_test.c" 2>&1) || {
            rc=$?
            fail "resurg codegen failed ($(describe_exit $rc))" "$RESURG $RG_FILE -o $BUILD/_test.c" "$STDERR"
        }
        STDERR=$($CC -std=c17 -Wno-tautological-compare -I"$RUNTIME" -o "$BUILD/_test" "$BUILD/_test.c" $RT_OBJS 2>&1) || {
            fail "C compilation failed" "$CC ... $BUILD/_test.c" "$STDERR"
        }
        STDERR=$("$BUILD/_test" 2>&1) || {
            rc=$?
            fail "test binary crashed ($(describe_exit $rc))" "$BUILD/_test" "$STDERR"
        }
        ;;

    compile_error)
        if STDERR=$("$RESURG" "$RG_FILE" -o "$BUILD/_test.c" 2>&1); then
            fail "expected compile error but resurg succeeded"
        fi
        if [[ -n "$EXPECT_ERROR" ]] && ! printf '%s' "$STDERR" | grep -qF "$EXPECT_ERROR"; then
            fail "expected error: $EXPECT_ERROR" "$RESURG $RG_FILE" "got: $STDERR"
        fi
        ;;

    runtime_error)
        STDERR=$("$RESURG" "$RG_FILE" -o "$BUILD/_test.c" 2>&1) || {
            rc=$?
            fail "resurg codegen failed ($(describe_exit $rc))" "$RESURG $RG_FILE -o $BUILD/_test.c" "$STDERR"
        }
        STDERR=$($CC -std=c17 -Wno-tautological-compare -I"$RUNTIME" -o "$BUILD/_test" "$BUILD/_test.c" $RT_OBJS 2>&1) || {
            fail "C compilation failed" "$CC ... $BUILD/_test.c" "$STDERR"
        }
        if "$BUILD/_test" 2>/dev/null; then
            fail "expected runtime error but program succeeded"
        fi
        ;;

    *)
        fail "unknown test mode: $TEST_MODE"
        ;;
esac
