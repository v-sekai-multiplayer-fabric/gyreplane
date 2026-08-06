# mud-sandbox-orchestrator

Raw-libriscv host binary that drives `mud/guest/mud_guest.cpp`'s
sandboxed Middleham MUD state machine. One process per active MUD
session, matching the sandboxed-godot-in-zone-server-h2o decision doc
(multiplayer-fabric-manuals PR #128).

## Protocol

Length-prefixed frames on stdin/stdout: a 4-byte little-endian length
followed by that many CBOR bytes. First frame in is the `mud_boot()`
config, every frame after is a `mud_step()` command. Each gets one
frame back, unmodified guest CBOR output.

## Build

Needs `libriscv` headers/lib and the generated `libriscv_settings.h`.
Both come from a `fabric-godot-core` checkout with the `sandbox`
module's `libriscv` submodule present
(`modules/sandbox/thirdparty/libriscv`), built once via its own
`emulator/build.sh` (produces `emulator/build/lib/libriscv.a` and
`emulator/build/lib/libriscv_settings.h`).

```sh
g++ -std=c++20 -O2 \
  -I "$LIBRISCV/lib" -I "$LIBRISCV/emulator/build/lib" \
  main.cpp "$LIBRISCV/emulator/build/lib/libriscv.a" \
  -o mud-sandbox-orchestrator -lpthread -ldl
```

## Guest build

`mud/guest/mud_guest.cpp` cross-compiles as a static riscv64-musl
executable (no libgodot dependency, unlike the Godot guest this same
orchestrator design targets next):

```sh
riscv64-buildroot-linux-musl-g++ -std=c++17 -O2 -static \
  -o mud_guest.rv64.elf mud_guest.cpp
```

## Manual test

```sh
./mud-sandbox-orchestrator ./mud_guest.rv64.elf
# then write length-prefixed CBOR frames to stdin, read them from stdout
```

Verified this way against a real riscv64-musl guest ELF under real
`libriscv` (not `qemu-riscv64`): `mud_boot`, then five real `mud_step`
turns (look/go/go/talk/talk), narration and room transitions matching
the native (non-sandboxed) smoke test byte for byte.
