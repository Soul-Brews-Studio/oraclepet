#!/usr/bin/env bash
# Build pet apps to wasm32. One ABI, four languages, picked by file extension:
#   .c   C              zig cc        (brew install zig)
#   .zig Zig            zig build-exe
#   .rs  Rust (no_std)  rustc --target wasm32-unknown-unknown  (rustup target add wasm32-unknown-unknown)
#   .ts  AssemblyScript asc           (cd assemblyscript && npm install)
#
#   ./build.sh examples/hello.c examples/hello-zig.zig examples/hello-rust.rs examples/hello-as.ts
#
# Every app gets the same small memory: one 64 KB page, 8 KB stack. Output: build/<name>.wasm
# Each one is then loaded by sim/build/petsim (the pet's WAMR on the Mac) as a check.
set -euo pipefail
cd "$(dirname "$0")"
mkdir -p build
MEM_LD=(--initial-memory=65536 --max-memory=65536 -zstack-size=8192)
for src in "$@"; do
  file=$(basename "$src"); name="${file%.*}"; ext="${file##*.}"
  [[ "$name" =~ ^[a-z0-9_-]{1,23}$ ]] || { echo "❌ $name: app names are lower-case a-z 0-9 - _ (max 23)"; exit 1; }
  out="build/$name.wasm"
  case "$ext" in
    c)
      # -fno-jump-tables: WAMR's br_table validator rejects the stack-polymorphic case LLVM
      # emits at -O2 (lab/gif-wamr fix #5)
      zig cc --target=wasm32-freestanding -O2 -fno-jump-tables -nostdlib -I. \
        -Wl,--no-entry -Wl,-z,stack-size=8192 -Wl,--initial-memory=65536 -Wl,--max-memory=65536 \
        -o "$out" "$src" ;;
    zig)
      zig build-exe -target wasm32-freestanding -O ReleaseSmall -fno-entry -rdynamic \
        --dep pet -Mroot="$src" -Mpet=zig/pet.zig \
        --stack 8192 --initial-memory=65536 --max-memory=65536 -femit-bin="$out"
      rm -f "build/$name.wasm.o" ;;
    rs)
      rustc=$(command -v rustc || echo "$HOME/.cargo/bin/rustc")
      # target-cpu=mvp: plain wasm1, nothing the pet's WAMR build might not enable
      "$rustc" --target wasm32-unknown-unknown --crate-type cdylib --edition 2021 \
        -C opt-level=s -C panic=abort -C target-cpu=mvp -C strip=symbols \
        $(printf -- '-C link-arg=%s ' "${MEM_LD[@]}") -o "$out" "$src" ;;
    ts)
      asc=assemblyscript/node_modules/.bin/asc
      [ -x "$asc" ] || (cd assemblyscript && npm install --silent)
      "$asc" "$src" -o "$out" -O3 --runtime incremental --noAssert \
        --initialMemory 1 --maximumMemory 1 --use abort=assemblyscript/pet/petAbort ;;
    *) echo "❌ $src: unknown language (.c .zig .rs .ts)"; exit 1 ;;
  esac
  echo "✅ $out  $(wc -c < "$out" | tr -d ' ') bytes  ($ext)"
  # Load it with the pet's own WAMR (same source + config, sim/): a module the pet would refuse
  # fails here instead of on the pet.
  if [ ! -x sim/build/petsim ]; then
    (cmake -S sim -B sim/build -DCMAKE_BUILD_TYPE=Release >/dev/null && cmake --build sim/build -j 8 >/dev/null) \
      || echo "   (petsim not built: build lab/jc3248-pet-idf once to fetch WAMR; skipping the load check)"
  fi
  [ -x sim/build/petsim ] && sim/build/petsim --check "$out" | sed 's/^/   /'
done
