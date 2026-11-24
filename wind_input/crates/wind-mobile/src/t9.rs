//! 9 键（T9）候选索引与输入会话（Phase 2 MVP）。
//!
//! 索引由 `WindInputAndroid/scripts/gen-t9.py` 从拼音词库生成，每行：
//! `数字序列<TAB>候选词<TAB>权重`。这里只做“数字串 → 候选词”的查表：
//! 不参与拼音整句/语法/学习，属于让 9 键先可用的最小实现；后续可并入引擎。
//!
//! 索引按数字串排序，支持精确匹配与前缀回退；候选按权重降序。

use std::collections::HashMap;
use std::path::Path;

/// 一条 T9 候选。
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct T9Candidate {
    pub text: String,
    pub weight: i64,
    /// 该候选词对应的全拼（无空格，如 `nihao`）；提交时回放给拼音引擎，
    /// 以走引擎的学习/联想。空串 = 索引没有该字段（旧数据）。
    pub pinyin: String,
}

/// T9 索引（只读）。
pub struct T9Index {
    /// 按数字串升序；每个数字串的候选已按权重降序、截断。
    entries: Vec<(String, Vec<T9Candidate>)>,
}

impl T9Index {
    pub fn empty() -> Self {
        Self {
            entries: Vec::new(),
        }
    }

    /// 从 `t9.txt` 加载。文件缺失/格式错误时返回空索引由调用方决定。
    pub fn load(path: &Path) -> std::io::Result<Self> {
        let text = std::fs::read_to_string(path)?;
        let mut map: HashMap<String, Vec<T9Candidate>> = HashMap::new();
        for line in text.lines() {
            let mut it = line.split('\t');
            let (Some(digits), Some(word)) = (it.next(), it.next()) else {
                continue;
            };
            if digits.is_empty() || word.is_empty() {
                continue;
            }
            let weight = it.next().and_then(|s| s.parse::<i64>().ok()).unwrap_or(0);
            let pinyin = it.next().unwrap_or("").to_string();
            map.entry(digits.to_string())
                .or_default()
                .push(T9Candidate {
                    text: word.to_string(),
                    weight,
                    pinyin,
                });
        }

        let mut entries: Vec<(String, Vec<T9Candidate>)> = map.into_iter().collect();
        entries.sort_by(|a, b| a.0.cmp(&b.0));
        for (_, items) in entries.iter_mut() {
            items.sort_by(|a, b| b.weight.cmp(&a.weight).then(a.text.cmp(&b.text)));
            items.truncate(30);
        }
        Ok(Self { entries })
    }

    /// 查询：先精确匹配；没有则按前缀回退（适合还没打完整个词的情况）。
    pub fn query(&self, digits: &str) -> Vec<T9Candidate> {
        if digits.is_empty() {
            return Vec::new();
        }
        match self.entries.binary_search_by(|(d, _)| d.as_str().cmp(digits)) {
            Ok(i) => self.entries[i].1.clone(),
            Err(i) => {
                let mut out = Vec::new();
                for (d, items) in self.entries[i..].iter() {
                    if !d.starts_with(digits) {
                        break;
                    }
                    out.extend(items.iter().cloned());
                }
                out.sort_by(|a, b| b.weight.cmp(&a.weight).then(a.text.cmp(&b.text)));
                out.truncate(30);
                out
            }
        }
    }
}

/// 一次 9 键输入会话（数字缓冲 + 索引）。
#[derive(Default)]
pub(crate) struct T9State {
    index: Option<T9Index>,
    buffer: String,
}

impl T9State {
    /// 后台预加载（启动线程调用）；已有索引则空操作。
    pub(crate) fn preload(&mut self, path: &Path) {
        if self.index.is_none() {
            self.index = Some(T9Index::load(path).unwrap_or_else(|_| T9Index::empty()));
        }
    }

    fn ensure(&mut self, path: &Path) -> &T9Index {
        if self.index.is_none() {
            self.index = Some(T9Index::load(path).unwrap_or_else(|_| T9Index::empty()));
        }
        self.index.as_ref().expect("T9 index just initialized")
    }

    pub(crate) fn push(&mut self, path: &Path, digit: char) -> Vec<T9Candidate> {
        if !('2'..='9').contains(&digit) {
            return self.candidates(path);
        }
        self.buffer.push(digit);
        self.candidates(path)
    }

    pub(crate) fn backspace(&mut self, path: &Path) -> Vec<T9Candidate> {
        self.buffer.pop();
        self.candidates(path)
    }

    pub(crate) fn clear(&mut self) {
        self.buffer.clear();
    }

    pub(crate) fn buffer_len(&self) -> usize {
        self.buffer.chars().count()
    }

    pub(crate) fn candidates(&mut self, path: &Path) -> Vec<T9Candidate> {
        let digits = self.buffer.clone();
        self.ensure(path).query(&digits)
    }
}
