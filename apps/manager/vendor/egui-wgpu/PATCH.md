# egui-wgpu 0.33.3

This source comes from the crates.io release at upstream commit `44cdd653e2317d300fb8a6c9c36b03f23991e803`. The original archive has SHA-256 `5e4d209971c84b2352a06174abdba701af1e552ce56b144d96f2bd50a3c91236`. Both upstream licenses accompany this copy. [Release](https://crates.io/crates/egui-wgpu/0.33.3) · [Upstream source](https://github.com/emilk/egui/tree/44cdd653e2317d300fb8a6c9c36b03f23991e803/crates/egui-wgpu)

The Action75 patch keeps the selected adapter in `RenderState::available_adapters` after renderer creation. Native selection still receives every candidate. Adapter diagnostics enumerate DXGI separately.

The manager's Cargo manifest resolves this copy through a relative path. The manager lockfile pins the full dependency graph for `cargo build --release --locked`.
