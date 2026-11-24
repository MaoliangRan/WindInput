//! 主机侧集成测试：不依赖 Android，直接验证 Rust 绑定层 + 引擎链路。
//!
//! 运行前需先准备词库：`WIND_BUILD_LOCAL=1 scripts/dev.sh gen-data`
//! （生成 `build_dev/data`；测试优先用它，缺省退回源 `data/`）。

use std::path::{Path, PathBuf};
use std::sync::{Arc, Mutex};

use wind_android::{
    AndroidClipboard, AndroidConfig, AndroidEventSink, FfiCandidateFrame, FfiEditOp, FfiInputStatus,
    FfiKeySource, FfiPressKey, WindAndroid,
};

#[derive(Default)]
struct TestSink {
    frames: Mutex<Vec<FfiCandidateFrame>>,
    status: Mutex<Option<FfiInputStatus>>,
    toasts: Mutex<Vec<String>>,
}

impl AndroidEventSink for TestSink {
    fn on_candidates(&self, frame: FfiCandidateFrame) {
        self.frames.lock().unwrap().push(frame);
    }

    fn on_hide_candidates(&self) {}

    fn on_status(&self, status: FfiInputStatus) {
        *self.status.lock().unwrap() = Some(status);
    }

    fn on_toast(&self, text: String) {
        self.toasts.lock().unwrap().push(text);
    }
}

#[derive(Default)]
struct TestClipboard {
    text: Mutex<String>,
}

impl AndroidClipboard for TestClipboard {
    fn set_text(&self, text: String) {
        *self.text.lock().unwrap() = text;
    }

    fn get_text(&self) -> String {
        self.text.lock().unwrap().clone()
    }

    fn get_text_cached(&self) -> String {
        self.text.lock().unwrap().clone()
    }
}

fn repo_data_dir() -> PathBuf {
    let root = Path::new(env!("CARGO_MANIFEST_DIR")).join("../../..");
    let generated = root.join("build_dev/data");
    if generated.join("schemas/pinyin/rime_frost.dict.yaml").exists() {
        generated
    } else {
        root.join("data")
    }
}

fn temp_dir(tag: &str) -> PathBuf {
    let dir = std::env::temp_dir().join(format!(
        "wind-android-test-{}-{}-{}",
        std::process::id(),
        tag,
        std::time::SystemTime::now()
            .duration_since(std::time::UNIX_EPOCH)
            .unwrap()
            .as_nanos()
    ));
    std::fs::create_dir_all(&dir).unwrap();
    dir
}

#[test]
fn pinyin_session_candidates_commit_and_phrases() {
    let data = repo_data_dir();
    assert!(
        data.join("schemas/pinyin/rime_frost.dict.yaml").exists(),
        "缺少拼音词库：{}（先运行 WIND_BUILD_LOCAL=1 scripts/dev.sh gen-data）",
        data.display()
    );

    let user = temp_dir("user");
    let local = temp_dir("local");
    let sink = Arc::new(TestSink::default());
    let clip = Arc::new(TestClipboard::default());

    let core = WindAndroid::new(
        AndroidConfig {
            data_dir: data.to_string_lossy().into_owned(),
            user_root: user.to_string_lossy().into_owned(),
            local_root: local.to_string_lossy().into_owned(),
            fallback_schema: "pinyin".into(),
        },
        sink.clone(),
        clip.clone(),
    );

    // 方案列表应包含 pinyin（语言键切回中文要用）。
    let schemas = core.schemas();
    let pinyin = schemas
        .iter()
        .position(|s| s.id == "pinyin")
        .unwrap_or_else(|| panic!("已装方案缺少 pinyin：{:?}", schemas.iter().map(|s| &s.id).collect::<Vec<_>>()));
    // 桌面 config 默认激活五笔；测试显式切到拼音（App 首次启动也会做同样的事）。
    core.run_command(wind_android::FfiMobileCommand::SelectSchema {
        index: pinyin as u32,
    });

    // 输入 nihao，应出候选。
    for ch in "nihao".chars() {
        core.press(
            FfiPressKey::Char {
                ch: ch.to_string(),
                shifted: false,
            },
            FfiKeySource::Touch,
        );
    }
    let py_start = std::time::Instant::now();
    let win = core.candidates(0, 20);
    assert!(!win.items.is_empty(), "拼音 nihao 应有候选");
    println!(
        "perf: 26 键候选 {} 条: {:?}",
        win.items.len(),
        py_start.elapsed()
    );

    // 选首个候选，应产出上屏/编辑指令。
    let out = core.select_candidate(0);
    assert!(
        out.ops
            .iter()
            .any(|op| matches!(op, FfiEditOp::Commit { .. })),
        "选首个候选应产出 Commit"
    );

    // 短语系统可用：系统短语 + 用户短语。
    let phrases = core.phrases();
    assert!(!phrases.is_empty(), "应加载系统短语");

    let code = "wtest";
    let text = "清风测试";
    core.phrase_add(code.into(), text.into()).expect("加短语");
    assert!(
        core.phrases()
            .iter()
            .any(|p| p.code == code && p.text == text),
        "加短语后应能在列表里看到"
    );
    core.phrase_remove(code.into(), text.into()).expect("删短语");

    // 9 键 T9：64 -> 你（索引由 scripts/gen-t9.py 生成）
    assert!(
        core.t9_available(),
        "缺少 t9.txt（先运行 scripts/gen-data.sh）"
    );
    let t9_start = std::time::Instant::now();
    assert!(
        !core.t9_push_digit("6".into()).is_empty(),
        "T9 首位应有候选"
    );
    println!("perf: T9 首次按键（含索引等待）: {:?}", t9_start.elapsed());
    let t9 = core.t9_push_digit("4".into());
    assert!(!t9.is_empty(), "T9 64 应有候选");
    assert!(!t9[0].pinyin.is_empty(), "T9 候选应带全拼");
    let fused_start = std::time::Instant::now();
    let fused = core.t9_candidates();
    assert!(!fused.is_empty(), "融合候选应非空");
    println!(
        "perf: T9 融合候选 {} 条: {:?}",
        fused.len(),
        fused_start.elapsed()
    );
    let t9_out = core.t9_commit_outcome(0).expect("T9 融合提交");
    assert!(
        t9_out
            .ops
            .iter()
            .any(|op| matches!(op, FfiEditOp::Commit { .. })),
        "T9 融合提交应产出 Commit"
    );
    assert_eq!(core.t9_buffer_len(), 0, "提交后数字缓冲应清空");

    // 剪贴板注入 / 内存回收 / 快照不应 panic。
    core.trim_memory();
    let _ = core.current_frame();
    let _ = core.current_status();
    let _ = core.readiness();

    drop(core);
    let _ = std::fs::remove_dir_all(&user);
    let _ = std::fs::remove_dir_all(&local);
}
