#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname "$0")/../.." && pwd)
g++ -std=c++17 -Wall -Werror -I "$root/examples/direct" \
  "$root/examples/direct/signaling_bind_test.cc" -o /tmp/signaling_bind_test
/tmp/signaling_bind_test
