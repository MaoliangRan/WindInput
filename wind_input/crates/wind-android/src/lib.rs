//! wind-android：Android 平台绑定层（UniFFI）。
//!
//! 本 crate 只做三件事：
//! 1. 把 `wind-mobile` 的移动端门面投影成 UniFFI 可导出的类型；
//! 2. 把 Kotlin 的事件回调/剪贴板实现桥接回核心；
//! 3. 装配 `MobileCore`（进程内，无 IPC）。
//!
//! # 为什么这里有镜像类型
//!
//! `wind-host` 的零依赖是硬约束，`wind-mobile` 也刻意不沾绑定框架，因此不能给
//! `EditOp`/`KeyOutcome` 等类型加 `uniffi` derive；而 UniFFI 不支持 `usize` 字段。
//! 本 crate 用 `Ffi*` 镜像类型做投影，转换处全部穷尽 `match`——新增变体会在这里编译失败，
//! 不会静默漏接。

uniffi::setup_scaffolding!();

use std::path::Path;
use std::sync::Arc;

use wind_host::{EditOp, KeyOutcome, Readiness, TimingHint};
use wind_mobile::{
    CandidateFrame, CandidateMenuItem, CandidateWindow, HostServices, InputStatus, KeySource,
    MobileCommand, MobileCore, MobileEventSink, Phrase, PressKey, SchemaEntry, T9Candidate,
    TextSegment,
};

// ─────────────────────────────────────────────────────────────────────────────
// Kotlin 侧实现的回调接口
// ─────────────────────────────────────────────────────────────────────────────

/// 候选/状态/提示回调（Rust 后台线程调用，Kotlin 实现方自行切主线程）。
#[uniffi::export(with_foreign)]
pub trait AndroidEventSink: Send + Sync {
    fn on_candidates(&self, frame: FfiCandidateFrame);
    fn on_hide_candidates(&self);
    fn on_status(&self, status: FfiInputStatus);
    fn on_toast(&self, text: String);
}

/// 剪贴板（Kotlin 实现，经 `HostServices` 注入核心）。
#[uniffi::export(with_foreign)]
pub trait AndroidClipboard: Send + Sync {
    fn set_text(&self, text: String);
    fn get_text(&self) -> String;
    /// 绝不阻塞；失败/不可读返回空串。
    fn get_text_cached(&self) -> String;
}

// ─────────────────────────────────────────────────────────────────────────────
// FFI 镜像类型
// ─────────────────────────────────────────────────────────────────────────────

/// 绑定层错误（UniFFI 要求 throw 类型实现 `uniffi::Error`；这里扁平化为一条消息）。
#[derive(Debug, thiserror::Error, uniffi::Error)]
#[uniffi(flat_error)]
pub enum FfiError {
    #[error("{message}")]
    Message { message: String },
}

/// 构造参数。三个目录语义见 `MobileCore::new`。
#[derive(uniffi::Record)]
pub struct AndroidConfig {
    /// 出厂数据目录（assets 解出的 data/）。
    pub data_dir: String,
    /// 用户目录（config/词库/短语/历史；私有 filesDir）。
    pub user_root: String,
    /// 本地目录（wdat 缓存/日志；cacheDir，可回收）。
    pub local_root: String,
    /// 首装兜底方案 id，默认 "pinyin"。
    pub fallback_schema: String,
}

/// 宿主无关编辑指令（镜像 `wind_host::EditOp`）。
#[derive(uniffi::Enum)]
pub enum FfiEditOp {
    Commit { text: String },
    SetComposition { text: String, caret: u32 },
    DeleteBackward { count: u32 },
    ReplaceBackward { count: u32, text: String },
    MoveCursor { delta: i32 },
}

/// TSF 时序提示（薄宿主可忽略）。
#[derive(uniffi::Enum)]
pub enum FfiTimingHint {
    AutoCommitAfter { timeout_ms: u32 },
    DeferCompositionUntilKeyUp { timeout_ms: u32 },
}

/// 一次按键的完整结果（镜像 `wind_host::KeyOutcome`）。
#[derive(uniffi::Record)]
pub struct FfiKeyOutcome {
    pub consumed: bool,
    pub ops: Vec<FfiEditOp>,
    pub hint: Option<FfiTimingHint>,
    pub mode_changed: bool,
}

/// 候选帧（镜像 `wind_mobile::CandidateFrame`）。
#[derive(uniffi::Record)]
pub struct FfiCandidateFrame {
    pub preedit: String,
    pub preedit_caret: u32,
    pub mode_label: String,
    pub composing: bool,
    pub has_candidates: bool,
    pub selected: u32,
    pub page: u32,
    pub total_pages: u32,
}

/// 模式真值（镜像 `wind_mobile::InputStatus`）。
#[derive(uniffi::Record)]
pub struct FfiInputStatus {
    pub chinese_mode: bool,
    pub latin_input: bool,
    pub icon_label: String,
    pub full_width: bool,
    pub chinese_punct: bool,
    pub s2t_enabled: bool,
    pub s2t_shown: bool,
}

/// 输入方案条目（镜像 `wind_mobile::SchemaEntry`）。
#[derive(uniffi::Record)]
pub struct FfiSchemaEntry {
    pub id: String,
    pub name: String,
    pub icon_label: String,
    pub active: bool,
}

/// 候选长按菜单项（镜像 `wind_mobile::CandidateMenuItem`）。
#[derive(uniffi::Record)]
pub struct FfiCandidateMenuItem {
    pub label: String,
    /// 原样回传给 `candidate_menu_run`（`move_top`/`delete`/`copy`…）。
    pub action: String,
    pub enabled: bool,
}

/// 命令短语（镜像 `wind_mobile::Phrase`）。
#[derive(uniffi::Record)]
pub struct FfiPhrase {
    pub preview: String,
    pub code: String,
    pub text: String,
    pub enabled: bool,
    pub is_system: bool,
}

/// 9 键候选（镜像 `wind_mobile::T9Candidate`）。
#[derive(uniffi::Record)]
pub struct FfiT9Candidate {
    pub text: String,
    pub weight: i64,
    pub pinyin: String,
}

/// 分段正文（镜像 `wind_mobile::TextSegment`）。
#[derive(uniffi::Record)]
pub struct FfiTextSegment {
    pub text: String,
    pub role: Option<String>,
}

/// 候选窗口（镜像 `wind_mobile::CandidateWindow`）。
#[derive(uniffi::Record)]
pub struct FfiCandidateWindow {
    pub items: Vec<String>,
    pub offset: u32,
    pub total: u32,
    pub selected: u32,
    pub comments: Vec<String>,
    pub text_segments: Vec<Vec<FfiTextSegment>>,
    pub rebuilt: bool,
}

/// 触摸键（镜像 `wind_mobile::PressKey`）。
#[derive(uniffi::Enum)]
pub enum FfiPressKey {
    Char { ch: String, shifted: bool },
    Space,
    Backspace,
    Enter,
}

/// 按键来源（镜像 `wind_mobile::KeySource`）。
#[derive(uniffi::Enum)]
pub enum FfiKeySource {
    Touch,
    Physical,
}

/// 就绪状态（镜像 `wind_host::Readiness`）。
#[derive(uniffi::Enum)]
pub enum FfiReadiness {
    Idle,
    Preparing,
    Ready,
    Failed,
}

/// 引擎命令（镜像 `wind_mobile::MobileCommand`）。
#[derive(uniffi::Enum)]
pub enum FfiMobileCommand {
    English,
    SelectSchema { index: u32 },
    TogglePunct,
    ToggleWidth,
    ToggleS2t,
    ReloadConfig,
    SetShuangpinLayout { id: String },
}

// ─────────────────────────────────────────────────────────────────────────────
// 转换：核心 → FFI
// ─────────────────────────────────────────────────────────────────────────────

impl From<EditOp> for FfiEditOp {
    fn from(op: EditOp) -> Self {
        match op {
            EditOp::Commit(text) => FfiEditOp::Commit { text },
            EditOp::SetComposition { text, caret } => FfiEditOp::SetComposition {
                text,
                caret: caret as u32,
            },
            EditOp::DeleteBackward { count } => FfiEditOp::DeleteBackward { count: count as u32 },
            EditOp::ReplaceBackward { count, text } => FfiEditOp::ReplaceBackward {
                count: count as u32,
                text,
            },
            EditOp::MoveCursor { delta } => FfiEditOp::MoveCursor { delta },
        }
    }
}

impl From<TimingHint> for FfiTimingHint {
    fn from(h: TimingHint) -> Self {
        match h {
            TimingHint::AutoCommitAfter { timeout_ms } => FfiTimingHint::AutoCommitAfter { timeout_ms },
            TimingHint::DeferCompositionUntilKeyUp { timeout_ms } => {
                FfiTimingHint::DeferCompositionUntilKeyUp { timeout_ms }
            }
        }
    }
}

impl From<KeyOutcome> for FfiKeyOutcome {
    fn from(o: KeyOutcome) -> Self {
        Self {
            consumed: o.consumed,
            ops: o.ops.into_iter().map(Into::into).collect(),
            hint: o.hint.map(Into::into),
            mode_changed: o.mode_changed,
        }
    }
}

impl From<CandidateFrame> for FfiCandidateFrame {
    fn from(f: CandidateFrame) -> Self {
        Self {
            preedit: f.preedit,
            preedit_caret: f.preedit_caret as u32,
            mode_label: f.mode_label,
            composing: f.composing,
            has_candidates: f.has_candidates,
            selected: f.selected as u32,
            page: f.page as u32,
            total_pages: f.total_pages as u32,
        }
    }
}

impl From<InputStatus> for FfiInputStatus {
    fn from(s: InputStatus) -> Self {
        Self {
            chinese_mode: s.chinese_mode,
            latin_input: s.latin_input,
            icon_label: s.icon_label,
            full_width: s.full_width,
            chinese_punct: s.chinese_punct,
            s2t_enabled: s.s2t_enabled,
            s2t_shown: s.s2t_shown,
        }
    }
}

impl From<SchemaEntry> for FfiSchemaEntry {
    fn from(s: SchemaEntry) -> Self {
        Self {
            id: s.id,
            name: s.name,
            icon_label: s.icon_label,
            active: s.active,
        }
    }
}

impl From<CandidateMenuItem> for FfiCandidateMenuItem {
    fn from(m: CandidateMenuItem) -> Self {
        Self {
            label: m.label,
            action: m.action,
            enabled: m.enabled,
        }
    }
}

impl From<Phrase> for FfiPhrase {
    fn from(p: Phrase) -> Self {
        Self {
            preview: p.preview,
            code: p.code,
            text: p.text,
            enabled: p.enabled,
            is_system: p.is_system,
        }
    }
}

impl From<T9Candidate> for FfiT9Candidate {
    fn from(c: T9Candidate) -> Self {
        Self {
            text: c.text,
            weight: c.weight,
            pinyin: c.pinyin,
        }
    }
}

impl From<TextSegment> for FfiTextSegment {
    fn from(s: TextSegment) -> Self {
        Self {
            text: s.text,
            role: s.role,
        }
    }
}

impl From<CandidateWindow> for FfiCandidateWindow {
    fn from(w: CandidateWindow) -> Self {
        Self {
            items: w.items,
            offset: w.offset as u32,
            total: w.total as u32,
            selected: w.selected as u32,
            comments: w.comments,
            text_segments: w
                .text_segments
                .into_iter()
                .map(|segs| segs.into_iter().map(Into::into).collect())
                .collect(),
            rebuilt: w.rebuilt,
        }
    }
}

impl From<Readiness> for FfiReadiness {
    fn from(r: Readiness) -> Self {
        match r {
            Readiness::Idle => FfiReadiness::Idle,
            Readiness::Preparing => FfiReadiness::Preparing,
            Readiness::Ready => FfiReadiness::Ready,
            Readiness::Failed => FfiReadiness::Failed,
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// 转换：FFI → 核心
// ─────────────────────────────────────────────────────────────────────────────

impl From<FfiPressKey> for PressKey {
    fn from(k: FfiPressKey) -> Self {
        match k {
            FfiPressKey::Char { ch, shifted } => PressKey::Char {
                ch: ch.chars().next().unwrap_or('\0'),
                shifted,
            },
            FfiPressKey::Space => PressKey::Space,
            FfiPressKey::Backspace => PressKey::Backspace,
            FfiPressKey::Enter => PressKey::Enter,
        }
    }
}

impl From<FfiKeySource> for KeySource {
    fn from(s: FfiKeySource) -> Self {
        match s {
            FfiKeySource::Touch => KeySource::Touch,
            FfiKeySource::Physical => KeySource::Physical,
        }
    }
}

impl From<FfiMobileCommand> for MobileCommand {
    fn from(c: FfiMobileCommand) -> Self {
        match c {
            FfiMobileCommand::English => MobileCommand::English,
            FfiMobileCommand::SelectSchema { index } => MobileCommand::SelectSchema(index as usize),
            FfiMobileCommand::TogglePunct => MobileCommand::TogglePunct,
            FfiMobileCommand::ToggleWidth => MobileCommand::ToggleWidth,
            FfiMobileCommand::ToggleS2t => MobileCommand::ToggleS2t,
            FfiMobileCommand::ReloadConfig => MobileCommand::ReloadConfig,
            FfiMobileCommand::SetShuangpinLayout { id } => MobileCommand::SetShuangpinLayout(id),
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// 桥接：MobileEventSink / HostServices
// ─────────────────────────────────────────────────────────────────────────────

struct EventSinkBridge(Arc<dyn AndroidEventSink>);

impl MobileEventSink for EventSinkBridge {
    fn on_candidates(&self, frame: CandidateFrame) {
        self.0.on_candidates(frame.into());
    }

    fn on_hide_candidates(&self) {
        self.0.on_hide_candidates();
    }

    fn on_status(&self, status: InputStatus) {
        self.0.on_status(status.into());
    }

    fn on_toast(&self, text: String) {
        self.0.on_toast(text);
    }
}

struct ClipboardBridge(Arc<dyn AndroidClipboard>);

impl HostServices for ClipboardBridge {
    fn clipboard_set_text(&self, text: &str) -> anyhow::Result<()> {
        self.0.set_text(text.to_string());
        Ok(())
    }

    fn clipboard_get_text(&self) -> anyhow::Result<String> {
        Ok(self.0.get_text())
    }

    fn clipboard_get_text_cached(&self) -> String {
        self.0.get_text_cached()
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// UniFFI 对象
// ─────────────────────────────────────────────────────────────────────────────

/// 一个实例对应宿主一个输入法服务生命周期。
#[derive(uniffi::Object)]
pub struct WindAndroid {
    core: Arc<MobileCore>,
}

#[uniffi::export]
impl WindAndroid {
    #[uniffi::constructor]
    pub fn new(
        cfg: AndroidConfig,
        sink: Arc<dyn AndroidEventSink>,
        clip: Arc<dyn AndroidClipboard>,
    ) -> Arc<Self> {
        let core = MobileCore::new(
            Path::new(&cfg.data_dir),
            Path::new(&cfg.user_root),
            Path::new(&cfg.local_root),
            &cfg.fallback_schema,
            Arc::new(EventSinkBridge(sink)),
        );
        // 必须在首个可能触碰剪贴板的调用之前注入（OnceLock，之后不再生效）。
        core.set_host_services(Arc::new(ClipboardBridge(clip)));
        Arc::new(Self { core })
    }

    /// 按下键（vk = wind-keys VK 码）。
    pub fn key_down(&self, vk: u32, modifiers: u32) -> FfiKeyOutcome {
        self.core.key_down(vk, modifiers).into()
    }

    /// 触摸键（字符/空格/退格/回车），由核心翻译 VK 并补英文空格。
    pub fn press(&self, key: FfiPressKey, source: FfiKeySource) -> FfiKeyOutcome {
        self.core.press(key.into(), source.into()).into()
    }

    /// 候选窗口（含注释/分段正文；`rebuilt=true` 时宿主须整份重取）。
    pub fn candidates(&self, offset: u32, limit: u32) -> FfiCandidateWindow {
        self.core
            .candidates_with_comments(offset as usize, limit as usize)
            .into()
    }

    /// 按绝对下标选候选。
    pub fn select_candidate(&self, index: u32) -> FfiKeyOutcome {
        self.core.select_candidate(index as usize).into()
    }

    /// 最近一帧（视图重建用；与推送同源）。
    pub fn current_frame(&self) -> Option<FfiCandidateFrame> {
        self.core.current_frame().map(Into::into)
    }

    /// 最近一次模式真值。
    pub fn current_status(&self) -> Option<FfiInputStatus> {
        self.core.current_status().map(Into::into)
    }

    /// 就绪状态。
    pub fn readiness(&self) -> FfiReadiness {
        self.core.readiness().into()
    }

    /// 引擎命令（中英/标点/全半角/简繁/方案切换/重载）。
    pub fn run_command(&self, cmd: FfiMobileCommand) {
        self.core.run_command(cmd.into());
    }

    /// 已装方案（语言键切回中文时按 id 找下标）。
    pub fn schemas(&self) -> Vec<FfiSchemaEntry> {
        self.core.schemas().into_iter().map(Into::into).collect()
    }

    /// 候选长按菜单（置顶/删除/屏蔽/复制等）。
    pub fn candidate_menu(&self, index: u32) -> Vec<FfiCandidateMenuItem> {
        self.core
            .candidate_menu(index as usize)
            .into_iter()
            .map(Into::into)
            .collect()
    }

    /// 执行候选菜单动作；`copy` 由宿主自己写剪贴板（核心不处理）。
    pub fn candidate_menu_run(&self, index: u32, action: String) -> bool {
        self.core.candidate_menu_run(index as usize, &action)
    }

    /// 命令短语（系统 + 用户叠加视图）。
    pub fn phrases(&self) -> Vec<FfiPhrase> {
        self.core.phrases().into_iter().map(Into::into).collect()
    }

    /// 加一条用户短语。
    pub fn phrase_add(&self, code: String, text: String) -> Result<(), FfiError> {
        self.core
            .phrase_add(&code, &text)
            .map_err(|message| FfiError::Message { message })
    }

    /// 删一条用户短语。
    pub fn phrase_remove(&self, code: String, text: String) -> Result<(), FfiError> {
        self.core
            .phrase_remove(&code, &text)
            .map_err(|message| FfiError::Message { message })
    }

    /// 开/关一条短语。
    pub fn phrase_set_enabled(
        &self,
        code: String,
        text: String,
        on: bool,
    ) -> Result<(), FfiError> {
        self.core
            .phrase_set_enabled(&code, &text, on)
            .map_err(|message| FfiError::Message { message })
    }

    /// 9 键索引是否可用（t9.txt 是否存在）。
    pub fn t9_available(&self) -> bool {
        self.core.t9_available()
    }

    /// 9 键：追加数字（"2".."9"），返回候选。
    pub fn t9_push_digit(&self, digit: String) -> Vec<FfiT9Candidate> {
        let ch = digit.chars().next().unwrap_or('\0');
        self.core
            .t9_push_digit(ch)
            .into_iter()
            .map(Into::into)
            .collect()
    }

    /// 9 键：退格一个数字，返回候选。
    pub fn t9_backspace(&self) -> Vec<FfiT9Candidate> {
        self.core
            .t9_backspace()
            .into_iter()
            .map(Into::into)
            .collect()
    }

    /// 9 键：清空数字缓冲。
    pub fn t9_clear(&self) {
        self.core.t9_clear();
    }

    /// 9 键：当前候选（融合拼音引擎）。
    pub fn t9_candidates(&self) -> Vec<FfiT9Candidate> {
        self.core
            .t9_candidates_fused()
            .into_iter()
            .map(Into::into)
            .collect()
    }

    /// 9 键：选择第 N 个候选，返回要上屏的文本（同时清空缓冲）。
    pub fn t9_commit(&self, index: u32) -> Option<String> {
        self.core.t9_commit(index as usize)
    }

    /// 9 键：选择第 N 个候选并走拼音引擎提交（学习/联想）。
    pub fn t9_commit_outcome(&self, index: u32) -> Option<FfiKeyOutcome> {
        self.core.t9_commit_outcome(index as usize).map(Into::into)
    }

    /// 9 键：当前数字缓冲长度。
    pub fn t9_buffer_len(&self) -> u32 {
        self.core.t9_buffer_len() as u32
    }

    /// 焦点获得（对应 onStartInput）。
    pub fn focus_gained(&self) {
        self.core.focus_gained();
    }

    /// 焦点丢失（对应 onFinishInput）。
    pub fn focus_lost(&self) {
        self.core.focus_lost();
    }

    /// 光标前文（对应 onUpdateSelection）。
    pub fn set_preceding_text(&self, text: String) {
        self.core.set_preceding_text(text);
    }

    /// 暂停/恢复学习（密码框/无痕）。
    pub fn set_learning_suspended(&self, on: bool) {
        self.core.set_learning_suspended(on);
    }

    /// 内存紧张时释放闲置资源（对应 onTrimMemory）。
    pub fn trim_memory(&self) {
        self.core.trim_memory();
    }
}
