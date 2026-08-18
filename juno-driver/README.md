# juno-driver-rti-t2i

The T2i as a Juno [`remote`](../docs/JUNO-INTEGRATION.md). Lives here, not in Juno: the key-code
table in [src/lib.rs](src/lib.rs) is exactly the RTI-specific knowledge Juno must not carry.

```bash
cargo test                                        # 13 tests, no hardware
RUSTC=~/.rustup/toolchains/stable-aarch64-apple-darwin/bin/rustc \
  ~/.rustup/toolchains/stable-aarch64-apple-darwin/bin/cargo \
  build --release --target wasm32-wasip1
```

**`RUSTC` has to be pinned on this machine.** `/opt/homebrew/bin/rustc` comes before rustup on
`PATH` and has no `wasm32-wasip1` std, so the build fails with `can't find crate for core` and
suggests installing a target that is already installed — even when invoked through rustup's own
cargo, because cargo still resolves `rustc` from `PATH`.

Validate the manifest against the contract:

```bash
cd ../../Juno/driver-sdk && cargo run --features=pack --bin junodrv -- check ../../GitHub/t2i-zephyr/juno-driver
```

`junodrv` bakes the contracts in at build time, so rebuild it after editing `proxies/*.toml` or it
will reject a capability that now exists.

## Why hold detection is in the firmware

A driver runs in a sandbox and **cannot read a clock**, so it can see `KEY DOWN` and `KEY UP` and
not the gap between them. The first version of this timed the hold here, which silently never
fired while the manifest claimed `has_hold = true` — the invisible failure core's own lint is
about. The remote now emits `KEY HELD <code> <name>` once at 400 ms (`KEY_HOLD_MS` in
[../src/main.c](../src/main.c)) and the driver maps the three edges onto `click` / `hold` /
`release`.
