#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""从 rime-frost 词库生成 9 键 T9 索引。

输入：<data>/schemas/pinyin/{rime_frost.dict.yaml, cn_dicts/*.dict.yaml, ext/*.dict.yaml}
输出：<data>/t9.txt，每行 `数字序列<TAB>候选词<TAB>权重`。

说明：
- 只做 Phase 2 MVP：按词条全拼转数字串，直接查候选；不参与拼音整句/学习。
- 每个数字串保留权重最高的若干条；全局按权重截断，控制体积。
- 词库来自第三方（不入库），生成物在 build_dev/data 下。
"""
import os
import re
import sys

DIGIT = {}
for letters, d in [
    ("abc", "2"), ("def", "3"), ("ghi", "4"), ("jkl", "5"),
    ("mno", "6"), ("pqrs", "7"), ("tuv", "8"), ("wxyz", "9"),
]:
    for ch in letters:
        DIGIT[ch] = d

CJK = re.compile(r"[\u3400-\u4dbf\u4e00-\u9fff]")


def flatten_pinyin(py: str):
    s = py.strip().lower().replace(" ", "").replace("'", "").replace("ü", "v")
    return s or None


def pinyin_to_digits(py: str):
    s = flatten_pinyin(py)
    if not s:
        return None
    out = []
    for ch in s:
        d = DIGIT.get(ch)
        if d is None:
            return None
        out.append(d)
    return "".join(out)


def parse_dict(path):
    """逐行解析 Rime 词库；跳过 YAML 头，返回 (text, pinyin, weight) 生成器。"""
    in_body = False
    with open(path, "r", encoding="utf-8", errors="ignore") as f:
        for line in f:
            line = line.rstrip("\n")
            if not in_body:
                if line.strip() == "...":
                    in_body = True
                continue
            if not line or line.startswith("#"):
                continue
            parts = line.split("\t")
            if len(parts) < 2:
                continue
            text = parts[0].strip()
            py = parts[1].strip()
            if not text or not py:
                continue
            if not CJK.search(text):
                continue
            if len(text) > 16:
                continue
            weight = 0
            if len(parts) >= 3:
                try:
                    weight = int(float(parts[2]))
                except ValueError:
                    weight = 0
            yield text, py, weight


def main():
    if len(sys.argv) != 2:
        print("用法: gen-t9.py <build_dev/data>", file=sys.stderr)
        sys.exit(2)
    data = sys.argv[1]
    pinyin_dir = os.path.join(data, "schemas", "pinyin")
    files = []
    main_dict = os.path.join(pinyin_dir, "rime_frost.dict.yaml")
    if os.path.exists(main_dict):
        files.append(main_dict)
    for sub in ("cn_dicts", "ext"):
        d = os.path.join(pinyin_dir, sub)
        if os.path.isdir(d):
            files.extend(
                os.path.join(d, n) for n in sorted(os.listdir(d))
                if n.endswith(".dict.yaml")
            )
    if not files:
        print(f"未找到拼音词库: {pinyin_dir}", file=sys.stderr)
        sys.exit(1)

    best = {}  # (digits, text) -> (weight, flat_pinyin)
    for path in files:
        for text, py, weight in parse_dict(path):
            digits = pinyin_to_digits(py)
            py_flat = flatten_pinyin(py)
            if not digits or not py_flat or len(digits) > 32:
                continue
            key = (digits, text)
            old = best.get(key)
            if old is None or weight > old[0]:
                best[key] = (weight, py_flat)

    # 每个数字串保留前 N；再按全局权重截断。
    per_digits = {}
    for (digits, text), (weight, py_flat) in best.items():
        per_digits.setdefault(digits, []).append((weight, text, py_flat))
    rows = []
    for digits, items in per_digits.items():
        items.sort(key=lambda x: (-x[0], x[1]))
        for weight, text, py_flat in items[:20]:
            rows.append((weight, digits, text, py_flat))

    rows.sort(key=lambda x: (-x[0], x[1], x[2]))
    rows = rows[:200000]

    out = os.path.join(data, "t9.txt")
    with open(out, "w", encoding="utf-8") as f:
        for weight, digits, text, py_flat in rows:
            f.write(f"{digits}\t{text}\t{weight}\t{py_flat}\n")
    print(f"t9: {len(rows)} 条 -> {out} ({os.path.getsize(out) / 1024 / 1024:.1f} MB)")


if __name__ == "__main__":
    main()
