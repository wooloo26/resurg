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
        "$RESURG" "$RG_FILE" -o "$BUILD/_test.c"
        $CC -std=c17 -I"$RUNTIME" -o "$BUILD/_test" "$BUILD/_test.c" $RT_OBJS
        "$BUILD/_test"
        ;;

    compile_error)
        if STDERR=$("$RESURG" "$RG_FILE" -o "$BUILD/_test.c" 2>&1); then
            echo "  FAIL  $RG_FILE — expected compile error but resurg succeeded"
            exit 1
        fi
        if [[ -n "$EXPECT_ERROR" ]] && ! printf '%s' "$STDERR" | grep -qF "$EXPECT_ERROR"; then
            echo "  FAIL  $RG_FILE — expected error: $EXPECT_ERROR"
            echo "         got: $STDERR"
            exit 1
        fi
        ;;

    runtime_error)
        "$RESURG" "$RG_FILE" -o "$BUILD/_test.c"
        $CC -std=c17 -I"$RUNTIME" -o "$BUILD/_test" "$BUILD/_test.c" $RT_OBJS
        if "$BUILD/_test" 2>/dev/null; then
            echo "  FAIL  $RG_FILE — expected runtime error but program succeeded"
            exit 1
        fi
        ;;

    *)
        echo "  FAIL  $RG_FILE — unknown test mode: $TEST_MODE"
        exit 1
        ;;
esac
