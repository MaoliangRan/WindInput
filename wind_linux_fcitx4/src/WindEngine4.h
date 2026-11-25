// 清风输入法的 Fcitx4 前端（Linux / X11）。
//
// 与 wind_linux（Fcitx5 addon）对位：同样是薄壳，按键经 UDS 交给 Rust 服务，候选窗使用
// 服务光栅化好的 SHM 帧。差异只在宿主 API：
//   - Fcitx4 没有 InputContext 对象化的预编辑面板/状态区，本端不设 fcitx 预编辑（服务端把
//     编码画进候选窗顶部栏，等价于 Fcitx5 的 inline_preedit=false）；
//   - Fcitx4 没有通用 addIOEvent，X11 面板与推送队列由 fcitx4 主线程的 10ms 定时器驱动
//     （CompatEvent.h），保证所有 UI / BridgeClient 调用都在主线程串行。
#pragma once

#include "Bridge.h"
#include "ExtProtocol.h"
#include "HostUi.h"
#include "KeyMap.h"
#include "KeySynth.h"
#include "OverlayInput.h"
#include "ResponseRouter.h"
#include "ServiceLauncher.h"
#include "SettingsLauncher.h"
#include "ShmFrame.h"
#include "CompatEvent.h"

#include <fcitx/addon.h>
#include <fcitx/fcitx.h>
#include <fcitx/frontend.h>
#include <fcitx/hook.h>
#include <fcitx/ime.h>
#include <fcitx/instance.h>

#include <memory>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

namespace windlinux {

class X11CandidatePanel;

class WindEngine4 {
public:
    explicit WindEngine4(FcitxInstance* instance);
    ~WindEngine4();

    // ── FcitxIMClass2 入口（Addon.cpp 注册）──
    static void* create(FcitxInstance* instance);
    static void destroy(void* arg);

private:
    // ── FcitxIMIFace 回调（arg = this）──
    static boolean initCb(void* arg);
    static void resetCb(void* arg);
    static INPUT_RETURN_VALUE doInputCb(void* arg, FcitxKeySym sym, unsigned int state);
    static INPUT_RETURN_VALUE doReleaseCb(void* arg, FcitxKeySym sym, unsigned int state);
    static INPUT_RETURN_VALUE getCandWordsCb(void* arg);
    static boolean phraseTipsCb(void* arg);
    static void saveCb(void* arg);
    static void reloadConfigCb(void* arg);
    static INPUT_RETURN_VALUE keyBlockerCb(void* arg, FcitxKeySym sym, unsigned int state);
    static void surroundingUpdatedCb(void* arg);
    static void onCloseCb(void* arg, FcitxIMCloseEventType type);
    static const char* subModeCb(void* arg);

    // ── 全局钩子 ──
    static void focusInHook(void* arg);
    static void focusOutHook(void* arg);
    static void imChangedHook(void* arg);
    static void tickCb(void* arg);

    // ── 生命周期 ──
    void attach(FcitxInputContext* ic, bool switchingIM);
    void detach(const char* reason, bool switchingIM);
    void resetState();
    FcitxInputContext* focusedIC();
    bool currentIMIsUs() const;
    void tick();
    void armTick();
    void disarmTick();
    void registerIM();

    // ── 连接 / 状态 ──
    bool ensureConnected();
    bool reconnect();
    bool requestWithRetry(const Bytes& frame, Frame& resp);
    bool sendAndDrain(const Bytes& frame);
    void sendAsync(const Bytes& frame);
    void noteStall();
    void resyncAfterStall();

    // ── 焦点 / 光标 ──
    static const char* programOf(FcitxInputContext* ic);
    uint64_t clientToken(FcitxInputContext* ic) const;
    uint64_t inputScopeMask(FcitxInputContext* ic) const;
    void sendFocusGained(FcitxInputContext* ic);
    void reportFocus(FcitxInputContext* ic);
    void sendHostDisplay(FcitxInputContext* ic);
    HostOutput hostOutput(FcitxInputContext* ic);
    void sendCaretUpdate(FcitxInputContext* ic);
    uint16_t prevCharFor(FcitxInputContext* ic);
    void noteHostCursor(FcitxInputContext* ic);
    /// 光标矩形（根窗口坐标）；宿主没报过时返回空。
    std::optional<Rect> caretRectOf(FcitxInputContext* ic) const;

    // ── 按键 ──
    INPUT_RETURN_VALUE doInput(FcitxKeySym sym, unsigned int state);
    INPUT_RETURN_VALUE doRelease(FcitxKeySym sym, unsigned int state);
    void sendKeyUp(FcitxInputContext* ic, uint32_t vk, uint32_t mods, uint8_t toggles);
    /// fcitx4 的按键 state 被 SimpleMask 剥掉了 CapsLock 位：这里把本端跟踪的锁定态补回去。
    uint8_t togglesFor(uint32_t state) const;
    /// fcitx4 的 FcitxHotkeyGetKey 会把「keysym 已表达」的 Shift 从 state 里删掉
    /// （Shift+.; → keysym '>'、state 无 Shift）。这里按 keysym + CapsLock 反推并补回。
    uint32_t modsFor(uint32_t sym, uint32_t state) const;
    bool sendAltEvent(FcitxInputContext* ic, uint32_t vk, uint8_t eventType, uint32_t mods,
                      uint8_t toggles);
    void sendCtrlRaw(FcitxInputContext* ic, uint32_t vk, uint8_t eventType, uint32_t mods,
                     uint8_t toggles);
    bool applyResponse(FcitxInputContext* ic, const Frame& resp, bool hostShortcut);
    void noteMode(const Frame& frame);

    // ── push ──
    void onPushFrame(Frame frame);
    void onRenderFrame(const HostRenderFramePayload& p);
    void onOverlayFrame(const OverlayFramePayload& p);
    void onMenuFrame(uint32_t level, const OverlayFramePayload& p);
    void onExt(const ExtEnvelope& ext);
    void launchSettings(const std::vector<std::string>& args);

    // ── 候选窗 / 浮层 ──
    void hideCandidate();
    void hidePanels();

    // ── 菜单 / 面板位置 / 全屏 ──
    void requestMenu(int32_t target, int32_t x, int32_t y, int32_t lx = 0, int32_t ly = 0);
    void reportPanelPos(uint32_t kind, int32_t x, int32_t y, const std::string& output);
    void reportFullscreen(bool fullscreen);

    // ── 命令直通车按键合成 ──
    void onKeySynth(const Frame& frame);
    void forwardKeys(FcitxInputContext* ic, const std::vector<SynthKey>& keys);
    void releaseHeldKeys(const char* reason);
    void armKeyHoldExpiry();

    FcitxInstance* instance_ = nullptr;
    ServiceLauncher launcher_;
    BridgeClient bridge_;
    ResponseRouter router_;
    ToggleTapDetector tap_;
    AltHold altHold_;
    CtrlRaw ctrlRaw_;
    /// fcitx4 把 LockMask 从按键 state 里剥掉（FcitxKeyState_SimpleMask 不含 CapsLock），
    /// 服务端只能靠每帧的 toggles 判断大写锁定：本端自己维护并在每帧补上。
    bool capsLockOn_ = false;
    bool candidatesVisible_ = false;
    uint16_t keySeq_ = 0;
    bool lastReportedSecure_ = false;
    bool serviceSeenOnce_ = false;
    bool focusUnreported_ = false;
    bool stallPending_ = false;
    bool untrustedWarned_ = false;
    bool active_ = false;      // 当前 IM 是清风且文本框有焦点
    bool wasCurrentIM_ = false; // 上一次 imChanged 时当前 IM 是否为我们
    FcitxInputContext* currentIC_ = nullptr;
    std::unordered_set<int> eatenKeys_;

    fcitx::EventLoop loop_;
    std::unique_ptr<fcitx::EventSourceTime> holdTimer_;
    std::unique_ptr<PushClient> push_;
    ShmFrameReader shm_;
    ShmFrameReader overlayShm_[3];
    ShmFrameReader menuShm_[OVERLAY_MENU_LEVELS];
    ShmFrameReader panelShm_[2];
    FullscreenReporter fullscreen_;
    std::unique_ptr<X11CandidatePanel> panel_;
    ModeIndicator mode_;
    std::string subMode_; // GetSubModeName 返回的内存（fcitx4 不接管所有权）
    KeyHoldTracker keyHolds_;
    SynthRateLimiter synthRate_;
    uint32_t keyHoldTimeoutMs_ = kDefaultKeyHoldTimeoutMs;
    FcitxInputContext* keyHoldIC_ = nullptr;
    std::unique_ptr<fcitx::EventSourceTime> keyHoldExpiry_;
    uint64_t tickTimeoutId_ = 0;
};

} // namespace windlinux
