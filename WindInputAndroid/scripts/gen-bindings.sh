#!/usr/bin/env bash
# 由 Rust cdylib 生成 Kotlin 绑定，写入 app/src/main/java/uniffi/wind_android。
# UniFFI 版本必须与 wind-android/Cargo.toml 完全一致。
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PROJECT="$(cd "$ROOT/.." && pwd)"
# shellcheck disable=SC1091
source "$PROJECT/.toolchain/env.sh" >/dev/null

cd "$PROJECT/wind_input"
# cli feature 只在 host 上开；Android 构建（build-rust.sh）不要开。
cargo build -p wind-android --features cli

OUT="$(mktemp -d)"
trap 'rm -rf "$OUT"' EXIT

# --metadata-no-deps：避免 cargo metadata 去解析/下载无关平台依赖。
CARGO_NET_OFFLINE=true ./target/debug/uniffi-bindgen generate \
  --library target/debug/libwind_android.so \
  --language kotlin --out-dir "$OUT" \
  --metadata-no-deps --no-format

mkdir -p "$ROOT/app/src/main/java/uniffi/wind_android"
cp "$OUT/uniffi/wind_android/wind_android.kt" \
   "$ROOT/app/src/main/java/uniffi/wind_android/wind_android.kt"
echo "[gen-bindings] 已更新 Kotlin 绑定"
