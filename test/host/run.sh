#!/bin/bash
# Copyright (c) 2026 SKL (Martin Abbott)
# SPDX-License-Identifier: MIT
#
# Host tests for SKL-OTA. Needs g++, python3 and a miniz 3.x source tree:
#   git clone -b 3.0.2 https://github.com/richgel999/miniz.git
#   MINIZ_DIR=path/to/miniz ./run.sh [firmware.bin]
# With no firmware.bin, a synthetic 1 MB image is used.
set -e
cd "$(dirname "$0")"
: "${MINIZ_DIR:?set MINIZ_DIR to a miniz 3.x source tree}"
mkdir -p build
[ -f build/miniz_export.h ] || echo '#define MINIZ_EXPORT' > build/miniz_export.h
IMG="${1:-build/synthetic.bin}"
if [ -z "$1" ]; then
  python3 -c "import random; random.seed(1); open('build/synthetic.bin','wb').write(bytes(random.choice(b'ESP32 firmware\x00\xff') for _ in range(1<<20)))"
fi
python3 -c "import sys,zlib; open('build/image.zz','wb').write(zlib.compress(open(sys.argv[1],'rb').read(), 9))" "$IMG"
gcc -O1 -w -Ibuild -I"$MINIZ_DIR" -c "$MINIZ_DIR/miniz_tinfl.c" -o build/miniz_tinfl.o
gcc -O1 -w -Ibuild -I"$MINIZ_DIR" -DMINIZ_NO_DEFLATE_APIS -c "$MINIZ_DIR/miniz.c" -o build/miniz.o
g++ -std=gnu++11 -O1 -w -Ibuild -I"$MINIZ_DIR" inflate_test.cpp build/miniz_tinfl.o build/miniz.o -o build/inflate_test
echo "== compressed download: streaming inflate =="
./build/inflate_test "$IMG" build/image.zz
