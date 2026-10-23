#!/bin/sh
set -e
cd "$(dirname "$0")"

# Each binary is written next to its sources (Ilangc/ilangc, Ilangdis/ilangdis, IlangVM/ilangvm). They are not put in
# the project folder itself: on a case-insensitive file system "ilangc" would be the same name as the folder "Ilangc".

FLAGS="-O3 -march=native -flto"
[ "$1" = "goto" ] && FLAGS="$FLAGS -DILANG_VM_COMPUTED_GOTO"
LIBS=-ldl

echo "[1/6] Building Ilangc..."
g++ -O3 -march=native -flto ./Ilangc/main.cpp -o Ilangc/ilangc

echo "[2/6] Building Ilangdis..."
gcc -O2 ./Ilangdis/*.c -o Ilangdis/ilangdis

echo "[3/6] Compiling PGO training sources..."
mkdir -p IlangVM/pgo
for f in IlangVM/pgo_src/*.i; do
    name=$(basename "$f" .i)
    echo "    $f -> IlangVM/pgo/$name.ic"
    ./Ilangc/ilangc -v "$f" -o "IlangVM/pgo/$name.ic"
done

echo "[4/6] Building the instrumented IlangVM..."
cd IlangVM
rm -f *.gcda
gcc $FLAGS -fprofile-generate -fprofile-update=single ./main.c ./VM/*.c -o ilangvm $LIBS

echo "[5/6] Running the training programs..."
for f in pgo/*.ic; do
    echo "    $f"
    ./ilangvm "$f" >/dev/null 2>&1 || true
done

echo "[6/6] Building the optimized IlangVM..."
gcc $FLAGS -fprofile-use -fprofile-correction -Wno-missing-profile ./main.c ./VM/*.c -o ilangvm $LIBS

rm -f *.gcda
cd ..
echo "Done: Ilangc/ilangc, Ilangdis/ilangdis and IlangVM/ilangvm built successfully."