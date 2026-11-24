#!/usr/bin/env bash
# 一键构建 debug APK：Rust .so → Kotlin 绑定 → Gradle。
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PROJECT="$(cd "$ROOT/.." && pwd)"
# shellcheck disable=SC1091
source "$PROJECT/.toolchain/env.sh" >/dev/null

if [ ! -f "$PROJECT/build_dev/data/schemas/pinyin/rime_frost.dict.yaml" ]; then
  echo "[build-apk] 警告：缺少 build_dev/data/schemas/pinyin/rime_frost.dict.yaml"
  echo "            拼音候选将不可用。先运行：$PROJECT/scripts/dev.sh gen-data"
fi

if [ "$#" -eq 0 ]; then
  "$ROOT/scripts/build-rust.sh" arm64-v8a
else
  "$ROOT/scripts/build-rust.sh" "$@"
fi
"$ROOT/scripts/gen-bindings.sh"

cd "$ROOT"
"$GRADLE_HOME/bin/gradle" :app:assembleDebug --no-daemon --console=plain
echo "[build-apk] 产物: $ROOT/app/build/outputs/apk/debug/app-debug.apk"
