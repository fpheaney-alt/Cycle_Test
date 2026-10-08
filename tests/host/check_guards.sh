#!/bin/sh
# Edits Config.h in a scratch copy with values that must be refused, and checks the compile fails with a
# readable message. usage: check_guards.sh <sketch dir> <c++ compiler> <warn flags>
SKETCH="$1"; CXX="$2"; WARN="$3"
DIR=build/guards
rm -rf "$DIR" && mkdir -p "$DIR"
bad=0
try() {   # try <description> <sed expression>
  rm -rf "$DIR/c" && mkdir -p "$DIR/c" && cp "$SKETCH"/* "$DIR/c/" && sed -i "$2" "$DIR/c/Config.h"
  if $CXX $WARN -Imocks -I"$DIR/c" sim.cpp -o "$DIR/x" > "$DIR/log.txt" 2>&1; then
    echo "  FAIL: accepted but must be refused: $1"; bad=1
  else
    echo "  refused: $1"
    echo "           -> $(grep -o 'static assertion failed: .*' "$DIR/log.txt" | head -1 | cut -c1-150)"
  fi
}
TRIM='s/\(SERVO_START_TRIM_DEG\[NUM_SERVOS\] *= *\){[^}]*}'
MEAS='s/\(SERVO_MEASURED_SWEEP_DEG\[NUM_SERVOS\] *= *\){[^}]*}'
try "S1 trim +60 degrees (end pulse would pass 2500 us)"   "$TRIM/\1{ 60.0f, 0.0f, 0.0f, 0.0f }/"
try "S4 trim -50 degrees (start pulse would be below 500 us)" "$TRIM/\1{ 0.0f, 0.0f, 0.0f, -50.0f }/"
try "S3 measured sweep 0 degrees"                          "$MEAS/\1{ 180.0f, 180.0f, 0.0f, 180.0f }/"
try "S2 measured sweep 80 degrees (implausible)"           "$MEAS/\1{ 180.0f, 80.0f, 180.0f, 180.0f }/"
try "S2 measured sweep 120 degrees (the corrected sweep would not fit in the travel)" "$MEAS/\1{ 180.0f, 120.0f, 180.0f, 180.0f }/"
[ "$bad" = 0 ] && echo "all five out-of-range edits were refused"
exit $bad
