#!/usr/bin/env bash
# 下载并组装第三方词库（rime-frost / pinyin-data / OpenCC 等）到 build_dev/data。
#
# 桌面仓的 scripts/dev.sh gen-data 默认会转发 Windows 编译机；这里用
# WIND_BUILD_LOCAL=1 强制本机执行（gen-data 不产出 DLL，纯数据组装）。
#
# 大文件先走 jsdelivr 预取（raw.githubusercontent 在部分网络下对大文件很慢）；
# 预取失败时 dev.sh 会回退 raw。tencent 词频不参与 assemble_data，缺失时写一个
# 最小合法占位，避免 17MB 下载卡住整个流程。
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PROJECT="$(cd "$ROOT/.." && pwd)"
# shellcheck disable=SC1091
source "$PROJECT/.toolchain/env.sh" >/dev/null

cd "$PROJECT"
mkdir -p .cache/rime-frost/cn_dicts .cache/rime-frost/en_dicts \
         .cache/pinyin-data .cache/opencc/dictionaries

FROST="https://cdn.jsdelivr.net/gh/gaboolic/rime-frost@master"
PINYIN="https://cdn.jsdelivr.net/gh/mozillazg/pinyin-data@master"
OPENCC="https://cdn.jsdelivr.net/gh/BYVoid/OpenCC@master/data/dictionary"

get() {
  [ -s "$2" ] && return 0
  echo "[prefetch] $(basename "$2")"
  curl -fsSL --retry 2 --retry-delay 2 --max-time 300 -o "$2" "$1" || {
    echo "[prefetch] 失败，交给 dev.sh 回退 raw: $1" >&2
    return 0
  }
}

get "$FROST/rime_frost.dict.yaml" .cache/rime-frost/rime_frost.dict.yaml
for f in 8105 41448 base ext others corrections; do
  get "$FROST/cn_dicts/$f.dict.yaml" ".cache/rime-frost/cn_dicts/$f.dict.yaml"
done
get "$FROST/en_dicts/en.dict.yaml" .cache/rime-frost/en_dicts/en.dict.yaml
get "$FROST/en_dicts/en_ext.dict.yaml" .cache/rime-frost/en_dicts/en_ext.dict.yaml
for f in pinyin kXHC1983 kTGHZ2013 kMandarin_8105 overwrite; do
  get "$PINYIN/$f.txt" ".cache/pinyin-data/$f.txt"
done
for f in STCharacters STPhrases TSCharacters TSPhrases TWVariants TWPhrases HKVariants; do
  get "$OPENCC/$f.txt" ".cache/opencc/dictionaries/$f.txt"
done

# tencent 不在 assemble_data 的复制清单里；上游文件约 17MB，raw 容易卡。
if [ ! -s .cache/rime-frost/cn_dicts/tencent.dict.yaml ]; then
  cat > .cache/rime-frost/cn_dicts/tencent.dict.yaml <<'EOF'
# 最小占位：Android 构建不需要 tencent 词频；gen-data 见到文件存在会跳过下载。
---
name: tencent
version: "1"
sort: by_weight
...
EOF
fi

WIND_BUILD_LOCAL=1 ./scripts/dev.sh gen-data

# 9 键 T9 索引（从拼音词库生成；缺少 python3 时 9 键不可用，不影响 26 键）
if command -v python3 >/dev/null 2>&1; then
  python3 -I "$ROOT/scripts/gen-t9.py" "$PROJECT/build_dev/data"
else
  echo "[gen-data] 未找到 python3，跳过 T9 索引生成（9 键不可用）" >&2
fi

echo "[gen-data] 完成：$PROJECT/build_dev/data"
echo "            Android 构建会自动优先使用该目录（含 rime-frost 拼音词库 + t9.txt）。"
