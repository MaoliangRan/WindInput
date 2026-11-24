#!/usr/bin/env bash
# 编译 Rust Android 绑定层（.so）并放入 app/src/main/jniLibs。
# 用法：scripts/build-rust.sh [abi...]，默认 arm64-v8a。
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PROJECT="$(cd "$ROOT/.." && pwd)"
# shellcheck disable=SC1091
source "$PROJECT/.toolchain/env.sh" >/dev/null

cd "$PROJECT/wind_input"

build_one() {
  local abi="$1" target="$2"
  echo "[build-rust] $abi ($target)"
  cargo build -p wind-android --release --target "$target"
  mkdir -p "$ROOT/app/src/main/jniLibs/$abi"
  cp "target/$target/release/libwind_android.so" \
     "$ROOT/app/src/main/jniLibs/$abi/libwind_android.so"
}

if [ "$#" -eq 0 ]; then
  build_one arm64-v8a aarch64-linux-android
else
  for abi in "$@"; do
    case "$abi" in
      arm64-v8a) build_one arm64-v8a aarch64-linux-android ;;
      *) echo "未知/已移除 ABI: $abi（当前只支持 arm64-v8a）" >&2; exit 2 ;;
    esac
  done
fi

echo "[build-rust] 完成"
