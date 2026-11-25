// 清风输入法 Fcitx4 前端实现。
//
// 逻辑骨架逐段对照 wind_linux/src/fcitx/WindEngine.cpp（Fcitx5 版），只替换宿主交互层：
//   - InputContext 上屏 / 周边文本 → FcitxInstanceCommitString / DeleteSurroundingText / ForwardKey
//   - Fcitx5 事件循环 → CompatEvent（fcitx4 主线程 10ms tick）
//   - 状态区 / 托盘 → 不做（fcitx4 classic UI 的输入法图标仍显示 IM 自己的 Icon）
// 按键协议、断线自愈、卡死熔断、候选窗交互、按键合成等逻辑与 Fcitx5 版保持一致。
#include "WindEngine4.h"

#include "Menu.h"
#include "Protocol.h"
#include "Utf.h"
#include "X11Panel.h"
#include "CompatLog.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <unistd.h>

FCITX_DEFINE_LOG_CATEGORY(windinput_log, "windinput");
#define WIND_DEBUG() FCITX_LOGC(windinput_log, Debug)
#define WIND_INFO() FCITX_LOGC(windinput_log, Info)
#define WIND_WARN() FCITX_LOGC(windinput_log, Warn)

namespace windlinux {

namespace {

uint64_t nowMs()
{
    using namespace std::chrono;
    return uint64_t(duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
}

/// 十六进制（不污染日志流格式标志）。
std::string hex(uint32_t v)
{
    char buf[16];
    std::snprintf(buf, sizeof(buf), "0x%x", v);
    return buf;
}

/// 任意修饰键（左右 Shift/Ctrl/Alt/Super/Hyper/Meta、CapsLock、NumLock、Mode_switch）。
bool isModifierSym(uint32_t sym)
{
    if (sym >= 0xFFE1u && sym <= 0xFFEEu) { // Shift_L … Hyper_R（含 Caps_Lock）
        return true;
    }
    switch (sym) {
    case 0xFF7Fu: // Num_Lock
    case 0xFF7Eu: // Mode_switch
    case 0xFE03u: // ISO_Level3_Shift
    case 0xFF20u: // Multi_key
        return true;
    default:
        return false;
    }
}

/// 光标前 / 后的码点（charIndex 按码点计）。越界或非法字节返回 -1。
int32_t codepointAt(const std::string& s, size_t charIndex)
{
    size_t i = 0;
    size_t n = 0;
    while (i < s.size() && n < charIndex) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        size_t len = 1;
        if ((c & 0x80) == 0) {
            len = 1;
        } else if ((c & 0xE0) == 0xC0) {
            len = 2;
        } else if ((c & 0xF0) == 0xE0) {
            len = 3;
        } else if ((c & 0xF8) == 0xF0) {
            len = 4;
        }
        if (i + len > s.size()) {
            return -1;
        }
        i += len;
        ++n;
    }
    if (i >= s.size()) {
        return 0; // 串尾：这一侧没有字符
    }
    const unsigned char c = static_cast<unsigned char>(s[i]);
    if ((c & 0x80) == 0) {
        return c;
    }
    if ((c & 0xE0) == 0xC0 && i + 1 < s.size()) {
        return ((c & 0x1F) << 6) | (static_cast<unsigned char>(s[i + 1]) & 0x3F);
    }
    if ((c & 0xF0) == 0xE0 && i + 2 < s.size()) {
        return ((c & 0x0F) << 12) | ((static_cast<unsigned char>(s[i + 1]) & 0x3F) << 6)
            | (static_cast<unsigned char>(s[i + 2]) & 0x3F);
    }
    if ((c & 0xF8) == 0xF0 && i + 3 < s.size()) {
        return ((c & 0x07) << 18) | ((static_cast<unsigned char>(s[i + 1]) & 0x3F) << 12)
            | ((static_cast<unsigned char>(s[i + 2]) & 0x3F) << 6)
            | (static_cast<unsigned char>(s[i + 3]) & 0x3F);
    }
    return -1;
}

/// 前 n 个码点的字节前缀（n 超过串长时返回全串）。
std::string prefixChars(const std::string& s, size_t n)
{
    size_t i = 0;
    size_t count = 0;
    while (i < s.size() && count < n) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        size_t len = 1;
        if ((c & 0xE0) == 0xC0) {
            len = 2;
        } else if ((c & 0xF0) == 0xE0) {
            len = 3;
        } else if ((c & 0xF8) == 0xF0) {
            len = 4;
        }
        i += len;
        ++count;
    }
    return s.substr(0, std::min(i, s.size()));
}

/// InputContext → TextSink 适配器（Fcitx4 版）。
class ICSink4 final : public TextSink {
public:
    ICSink4(FcitxInstance* instance, FcitxInputContext* ic) : instance_(instance), ic_(ic) { }

    void commitText(const std::string& utf8) override
    {
        if (!utf8.empty()) {
            FcitxInstanceCommitString(instance_, ic_, utf8.c_str());
        }
    }

    void setPreedit(const std::string&, size_t) override
    {
        // 不设 fcitx4 预编辑：服务端已按 inline_preedit=false 把编码画进候选窗顶部栏。
        // 设了反而会多出一个 classic UI 的预编辑小窗压在候选窗下面。
    }

    bool deleteBeforeCursor(size_t utf16Units) override
    {
        if (!(ic_->contextCaps & CAPACITY_SURROUNDING_TEXT)) {
            return false;
        }
        size_t chars = utf16Units;
        char* str = nullptr;
        unsigned int cursor = 0;
        unsigned int anchor = 0;
        if (FcitxInstanceGetSurroundingText(instance_, ic_, &str, &cursor, &anchor) && str) {
            const std::string full(str);
            std::free(str);
            const size_t total = utf8CharCount(full);
            if (cursor <= total) {
                chars = codepointsForUtf16Back(prefixChars(full, cursor), utf16Units);
            }
        }
        if (chars > 0) {
            FcitxInstanceDeleteSurroundingText(instance_, ic_, -int(chars), unsigned(chars));
        }
        return true;
    }

    void moveCursor(int chars) override
    {
        const FcitxKeySym sym = chars < 0 ? FcitxKey_Left : FcitxKey_Right;
        const int n = chars < 0 ? -chars : chars;
        for (int i = 0; i < n && i < 64; ++i) {
            FcitxInstanceForwardKey(instance_, ic_, FCITX_PRESS_KEY, sym, 0);
            FcitxInstanceForwardKey(instance_, ic_, FCITX_RELEASE_KEY, sym, 0);
        }
    }

private:
    FcitxInstance* instance_;
    FcitxInputContext* ic_;
};

} // namespace

// ── 创建 / 销毁 ──────────────────────────────────────────────────────

void* WindEngine4::create(FcitxInstance* instance)
{
    return new WindEngine4(instance);
}

void WindEngine4::destroy(void* arg)
{
    delete static_cast<WindEngine4*>(arg);
}

WindEngine4::WindEngine4(FcitxInstance* instance) : instance_(instance)
{
    if (!ensureIconDirs(userHicolorDir())) {
        WIND_WARN() << "用户图标目录建不起来，候选窗图标可能缺失";
    }

    keyHoldTimeoutMs_ = keyHoldTimeoutMs(std::getenv("WIND_KEY_HOLD_TIMEOUT_MS"));
    router_.setTimerScheduler([this](uint32_t ms, std::function<void()> cb) {
        holdTimer_ = loop_.addTimeEvent(
            CLOCK_MONOTONIC, fcitx::now(CLOCK_MONOTONIC) + uint64_t(ms) * 1000, 0,
            [cb = std::move(cb)](fcitx::EventSourceTime*, uint64_t) {
                cb();
                return true;
            });
    });
    router_.setSinkProvider([this](const std::function<void(TextSink*)>& use) {
        if (FcitxInputContext* ic = focusedIC()) {
            ICSink4 sink(instance_, ic);
            use(&sink);
        } else {
            use(nullptr);
        }
    });

    // 候选窗交互：X11 事件在 fcitx4 主线程 tick 里处理，与按键串行，可直接复用按键那条连接。
    X11CandidatePanel::Callbacks cb;
    cb.select = [this](int32_t i) { sendAndDrain(encodeCandidateSelectFrame(i)); };
    cb.hover = [this](int32_t i) { sendAndDrain(encodeCandidateHoverFrame(i)); };
    cb.scroll = [this](int32_t d) { sendAndDrain(encodeCandidateScrollFrame(d)); };
    cb.contextMenu = [this](int32_t target, int32_t x, int32_t y) { requestMenu(target, x, y); };
    cb.menuPointer = [this](uint32_t e, uint32_t b, int32_t x, int32_t y) {
        if (!sendAndDrain(encodeMenuPointerFrame(e, b, x, y))) {
            WIND_WARN() << "菜单指针事件报不上去，本端收起菜单、放开指针";
            if (panel_) {
                panel_->closeMenu(nullptr);
            }
        }
    };
    cb.menuDismissed = [this](const std::string& reason) {
        sendAndDrain(encodeMenuDismissFrame(reason));
    };
    cb.overlayMenu = [this](int32_t target, int32_t x, int32_t y, int32_t lx, int32_t ly) {
        requestMenu(target, x, y, lx, ly);
    };
    cb.statusMoved = [this](int32_t x, int32_t y) {
        sendAndDrain(encodePosFrame(EXT_KIND_POS_STATUS_TIP, x, y));
    };
    cb.panelPointer = [this](uint32_t kind, uint32_t ev, uint32_t b, int32_t x, int32_t y) {
        sendAndDrain(encodePanelPointer(kind, ev, b, x, y));
    };
    cb.panelMoved = [this](uint32_t kind, int32_t x, int32_t y, const std::string& output) {
        reportPanelPos(kind, x, y, output);
    };
    cb.fullscreenChanged = [this](bool on) { reportFullscreen(on); };
    cb.decorClick = [this](uint32_t i) { sendAndDrain(encodeDecorClickFrame(i)); };
    cb.candidateMoved = [this](int32_t x, int32_t y) {
        sendAndDrain(encodePosFrame(EXT_KIND_POS_CANDIDATE, x, y));
    };
    panel_ = std::make_unique<X11CandidatePanel>(loop_, std::move(cb));

    // 推送线程 → 主线程（tick 里由 EventLoop 执行）。
    push_ = std::make_unique<PushClient>(
        pushSocketPath(),
        [this](Frame f) {
            loop_.post([this, f = std::move(f)]() mutable { onPushFrame(std::move(f)); });
        },
        [this](bool connected) {
            if (!connected) {
                loop_.post([this]() {
                    if (panel_) {
                        panel_->closeMenu(nullptr);
                    }
                    releaseHeldKeys("push_lost");
                });
            }
        });
    push_->start();

    registerIM();
    armTick();
    ensureConnected();
    WIND_INFO() << "WindInput Fcitx4 引擎已加载，服务端点 " << requestSocketPath();
}

WindEngine4::~WindEngine4()
{
    disarmTick();
    releaseHeldKeys("addon_unload");
    keyHoldExpiry_.reset();
    push_.reset();
    panel_.reset();
    holdTimer_.reset();
}

void WindEngine4::registerIM()
{
    FcitxIMIFace iface{};
    iface.Init = &WindEngine4::initCb;
    iface.ResetIM = &WindEngine4::resetCb;
    iface.DoInput = &WindEngine4::doInputCb;
    iface.GetCandWords = &WindEngine4::getCandWordsCb;
    iface.PhraseTips = &WindEngine4::phraseTipsCb;
    iface.Save = &WindEngine4::saveCb;
    iface.ReloadConfig = &WindEngine4::reloadConfigCb;
    iface.KeyBlocker = &WindEngine4::keyBlockerCb;
    iface.UpdateSurroundingText = &WindEngine4::surroundingUpdatedCb;
    iface.DoReleaseInput = &WindEngine4::doReleaseCb;
    iface.OnClose = &WindEngine4::onCloseCb;
    iface.GetSubModeName = &WindEngine4::subModeCb;

    // fcitx4 的 FcitxInstanceRegisterIMv2 对 priority <= 0 直接 return（静默不注册）：
    // 取与内置拼音相同的 5，避免出现在列表末尾。
    FcitxInstanceRegisterIMv2(instance_, this, "windinput", "清风输入法", "windinput", iface, 5,
                              "zh_CN");

    FcitxIMEventHook focusIn{&WindEngine4::focusInHook, this};
    FcitxIMEventHook focusOut{&WindEngine4::focusOutHook, this};
    FcitxIMEventHook imChanged{&WindEngine4::imChangedHook, this};
    FcitxInstanceRegisterInputFocusHook(instance_, focusIn);
    FcitxInstanceRegisterInputUnFocusHook(instance_, focusOut);
    FcitxInstanceRegisterIMChangedHook(instance_, imChanged);
}

void WindEngine4::armTick()
{
    tickTimeoutId_ = FcitxInstanceAddTimeout(instance_, 10, &WindEngine4::tickCb, this);
}

void WindEngine4::disarmTick()
{
    if (tickTimeoutId_ != 0) {
        FcitxInstanceRemoveTimeoutById(instance_, tickTimeoutId_);
        tickTimeoutId_ = 0;
    }
}

void WindEngine4::tickCb(void* arg)
{
    static_cast<WindEngine4*>(arg)->tick();
}

void WindEngine4::tick()
{
    loop_.runOnce();
    armTick();
}

// ── IM 回调 ──────────────────────────────────────────────────────────

boolean WindEngine4::initCb(void* arg)
{
    auto* self = static_cast<WindEngine4*>(arg);
    self->wasCurrentIM_ = true;
    if (!self->active_) {
        if (FcitxInputContext* ic = FcitxInstanceGetCurrentIC(self->instance_)) {
            self->attach(ic, true);
        }
    }
    return true;
}

void WindEngine4::resetCb(void* arg)
{
    auto* self = static_cast<WindEngine4*>(arg);
    self->resetState();
}

INPUT_RETURN_VALUE WindEngine4::doInputCb(void* arg, FcitxKeySym sym, unsigned int state)
{
    return static_cast<WindEngine4*>(arg)->doInput(sym, state);
}

INPUT_RETURN_VALUE WindEngine4::doReleaseCb(void* arg, FcitxKeySym sym, unsigned int state)
{
    return static_cast<WindEngine4*>(arg)->doRelease(sym, state);
}

INPUT_RETURN_VALUE WindEngine4::getCandWordsCb(void*)
{
    // 候选完全由服务端光栅化、本端贴图；不走 fcitx4 的候选词表。
    return IRV_DO_NOTHING;
}

boolean WindEngine4::phraseTipsCb(void*)
{
    return false;
}

void WindEngine4::saveCb(void*) { }

void WindEngine4::reloadConfigCb(void*) { }

INPUT_RETURN_VALUE WindEngine4::keyBlockerCb(void*, FcitxKeySym, unsigned int)
{
    return IRV_TO_PROCESS;
}

void WindEngine4::surroundingUpdatedCb(void*) { }

void WindEngine4::onCloseCb(void* arg, FcitxIMCloseEventType type)
{
    auto* self = static_cast<WindEngine4*>(arg);
    const bool switching = type == CET_SwitchIM;
    self->detach(switching ? "switch_im" : "close", switching);
}

const char* WindEngine4::subModeCb(void* arg)
{
    auto* self = static_cast<WindEngine4*>(arg);
    self->subMode_ = self->mode_.subModeName();
    return self->subMode_.c_str();
}

void WindEngine4::focusInHook(void* arg)
{
    auto* self = static_cast<WindEngine4*>(arg);
    if (!self->currentIMIsUs()) {
        return;
    }
    if (FcitxInputContext* ic = FcitxInstanceGetCurrentIC(self->instance_)) {
        self->attach(ic, false);
    }
}

void WindEngine4::focusOutHook(void* arg)
{
    auto* self = static_cast<WindEngine4*>(arg);
    if (self->active_) {
        self->detach("focus_out", false);
    }
}

void WindEngine4::imChangedHook(void* arg)
{
    auto* self = static_cast<WindEngine4*>(arg);
    const bool nowUs = self->currentIMIsUs();
    if (nowUs && !self->wasCurrentIM_) {
        if (FcitxInputContext* ic = FcitxInstanceGetCurrentIC(self->instance_)) {
            self->attach(ic, true);
        }
    } else if (!nowUs && self->wasCurrentIM_) {
        self->detach("im_changed", true);
    }
    self->wasCurrentIM_ = nowUs;
}

// ── 焦点 / 生命周期 ──────────────────────────────────────────────────

bool WindEngine4::currentIMIsUs() const
{
    FcitxIM* im = FcitxInstanceGetCurrentIM(instance_);
    return im && im->uniqueName && std::strcmp(im->uniqueName, "windinput") == 0;
}

FcitxInputContext* WindEngine4::focusedIC()
{
    if (!active_ || !currentIC_) {
        return nullptr;
    }
    if (FcitxInstanceGetCurrentIC(instance_) != currentIC_) {
        return nullptr;
    }
    if (currentIC_->state == IS_CLOSED) {
        return nullptr;
    }
    return currentIC_;
}

void WindEngine4::attach(FcitxInputContext* ic, bool switchingIM)
{
    if (!ic) {
        return;
    }
    if (active_ && currentIC_ == ic) {
        if (switchingIM) {
            sendAsync(encodeClientTokenFrame(CMD_IME_ACTIVATED, clientToken(ic)));
        }
        return;
    }
    if (active_ && currentIC_ && currentIC_ != ic) {
        detach("focus_changed", false);
    }
    currentIC_ = ic;
    active_ = true;
    router_.reset();
    tap_.reset();
    altHold_.reset();
    ctrlRaw_.reset();
    // fcitx4 不把 LockMask 交给我们：焦点进入时向 X 服务器查一次真实锁定态。
    if (panel_) {
        if (auto on = panel_->capsLockOn()) {
            capsLockOn_ = *on;
        }
    }
    if (mode_.noteCapsLock(capsLockOn_)) {
        subMode_ = mode_.subModeName();
    }
    eatenKeys_.clear();
    if (!ensureConnected()) {
        focusUnreported_ = true;
        return;
    }
    if (switchingIM) {
        sendAsync(encodeClientTokenFrame(CMD_IME_ACTIVATED, clientToken(ic)));
    }
    reportFocus(ic);
}

void WindEngine4::detach(const char* reason, bool switchingIM)
{
    FcitxInputContext* ic = currentIC_;
    if (!ic && !active_) {
        return;
    }
    active_ = false;
    focusUnreported_ = false;
    releaseHeldKeys(reason);
    if (ic) {
        if (uint32_t rvk = ctrlRaw_.takeHeld(); rvk != 0 && bridge_.isConnected()) {
            sendCtrlRaw(ic, rvk, KEY_EVENT_MOD_UP, KEYMOD_CTRL, TOGGLE_MOD_NOT_TAP);
        }
    }
    tap_.reset();
    altHold_.reset();
    if (panel_) {
        panel_->closeMenu(reason);
    }
    if (router_.hasComposition() && ic) {
        ICSink4 sink(instance_, ic);
        router_.applyClearComposition(&sink);
    } else {
        router_.reset();
    }
    eatenKeys_.clear();
    if (bridge_.isConnected() && ic) {
        const uint64_t token = clientToken(ic);
        sendAndDrain(encodeFocusLostFrame(token, FOCUS_LOST_REASON_THREAD));
        if (switchingIM) {
            sendAsync(encodeClientTokenFrame(CMD_IME_DEACTIVATED, token));
        }
    }
    currentIC_ = nullptr;
}

void WindEngine4::resetState()
{
    releaseHeldKeys("reset");
    router_.seqTracker().reset();
    if (!router_.hasComposition()) {
        return;
    }
    if (FcitxInputContext* ic = focusedIC()) {
        ICSink4 sink(instance_, ic);
        router_.applyClearComposition(&sink);
    }
    if (bridge_.isConnected()) {
        sendAsync(encodeEmptyFrame(CMD_COMPOSITION_TERMINATED, true));
    }
}

// ── 连接 ────────────────────────────────────────────────────────────

bool WindEngine4::ensureConnected()
{
    if (bridge_.isConnected()) {
        return true;
    }
    return reconnect();
}

bool WindEngine4::reconnect()
{
    if (bridge_.connect(requestSocketPath())) {
        WIND_INFO() << "已连上服务";
        noteStall();
        if (stallPending_) {
            stallPending_ = false;
            resyncAfterStall();
        }
        if (focusUnreported_) {
            if (FcitxInputContext* ic = focusedIC()) {
                reportFocus(ic);
            }
            focusUnreported_ = false;
        }
        return true;
    }
    switch (bridge_.lastFailure()) {
    case BridgeClient::Failure::Suspended:
        return false;
    case BridgeClient::Failure::ConnectTimeout:
        WIND_WARN() << "服务无响应（连接排队超时），按键暂时透传";
        noteStall();
        return false;
    case BridgeClient::Failure::Untrusted:
        if (!untrustedWarned_) {
            WIND_WARN() << bridge_.lastError();
            untrustedWarned_ = true;
        }
        return false;
    default:
        break;
    }
    WIND_DEBUG() << "连不上服务: " << bridge_.lastError();
    launcher_.maybeLaunch();
    return false;
}

bool WindEngine4::requestWithRetry(const Bytes& frame, Frame& resp)
{
    if (windlinux::requestWithRetry(bridge_, frame, resp, [this] { return reconnect(); })) {
        return true;
    }
    if (bridge_.lastFailure() != BridgeClient::Failure::Suspended) {
        WIND_WARN() << "服务 I/O 失败（" << bridge_.lastError() << "），本键透传";
    }
    noteStall();
    return false;
}

bool WindEngine4::sendAndDrain(const Bytes& frame)
{
    Frame ignored;
    if (!ensureConnected() || !bridge_.request(frame, ignored)) {
        WIND_DEBUG() << "发送失败: " << bridge_.lastError();
        noteStall();
        return false;
    }
    return true;
}

void WindEngine4::sendAsync(const Bytes& frame)
{
    if (!ensureConnected() || !bridge_.send(frame)) {
        WIND_DEBUG() << "异步发送失败: " << bridge_.lastError();
        noteStall();
    }
}

void WindEngine4::noteStall()
{
    if (!bridge_.takeStallFlag() || stallPending_) {
        return;
    }
    WIND_WARN() << "服务无响应：本端收起组字与候选窗，恢复前按键透传";
    stallPending_ = true;
    if (FcitxInputContext* ic = focusedIC(); ic && router_.hasComposition()) {
        ICSink4 sink(instance_, ic);
        router_.applyClearComposition(&sink);
    } else {
        router_.reset();
    }
    if (panel_) {
        panel_->closeMenu(nullptr);
    }
    hideCandidate();
    if (panel_) {
        panel_->hideAllOverlays();
    }
    releaseHeldKeys("service_stalled");
}

void WindEngine4::resyncAfterStall()
{
    WIND_INFO() << "服务恢复响应，对齐组字与焦点状态";
    sendAsync(encodeEmptyFrame(CMD_COMPOSITION_TERMINATED, true));
    if (FcitxInputContext* ic = focusedIC()) {
        sendFocusGained(ic);
    }
}

// ── 焦点 / 光标上报 ──────────────────────────────────────────────────

const char* WindEngine4::programOf(FcitxInputContext* ic)
{
    if (!ic) {
        return "";
    }
    // fcitx4.2.9 的 IC 实际都是 FcitxInputContext2（结构头部相同），prgname 由前端填。
    auto* ic2 = reinterpret_cast<FcitxInputContext2*>(ic);
    return ic2->prgname ? ic2->prgname : "";
}

uint64_t WindEngine4::clientToken(FcitxInputContext* ic) const
{
    const uint32_t host = stableHash(programOf(ic));
    const uint32_t low = stableHash(std::to_string(reinterpret_cast<uintptr_t>(ic)));
    return (uint64_t(host) << 32) | low;
}

uint64_t WindEngine4::inputScopeMask(FcitxInputContext* ic) const
{
    return (ic && (ic->contextCaps & CAPACITY_PASSWORD)) ? INPUT_SCOPE_PASSWORD_BIT : 0;
}

std::optional<Rect> WindEngine4::caretRectOf(FcitxInputContext* ic) const
{
    if (!ic) {
        return std::nullopt;
    }
    int x = 0;
    int y = 0;
    int w = 0;
    int h = 0;
    FcitxInstanceGetWindowRect(instance_, ic, &x, &y, &w, &h);
    if (x == 0 && y == 0 && w == 0 && h == 0) {
        return std::nullopt;
    }
    return Rect{x, y, w, h > 0 ? h : 16};
}

void WindEngine4::sendHostDisplay(FcitxInputContext* ic)
{
    if (!ensureConnected()) {
        return;
    }
    // Fcitx4 走 X11：光标坐标可信；本端不设 fcitx 预编辑，inline_preedit 恒 false。
    sendAndDrain(encodeHostDisplayFrame(false, true, false, panel_ ? panel_->hostScale() : 1.0,
                                        hostOutput(ic)));
}

HostOutput WindEngine4::hostOutput(FcitxInputContext* ic)
{
    HostOutput out;
    std::optional<std::pair<int32_t, int32_t>> at;
    if (auto r = caretRectOf(ic)) {
        at = std::make_pair(r->x, r->y);
    } else if (panel_) {
        at = panel_->pointerPosition();
    }
    if (panel_ && at) {
        if (std::optional<HostOutput> o = panel_->outputAt(at->first, at->second)) {
            return *o;
        }
    }
    if (panel_) {
        out.work = panel_->screenWorkArea();
    }
    return out;
}

void WindEngine4::sendFocusGained(FcitxInputContext* ic)
{
    const uint64_t mask = inputScopeMask(ic);
    lastReportedSecure_ = mask != 0;
    sendHostDisplay(ic);
    Frame resp;
    if (ensureConnected()
        && bridge_.request(encodeFocusGainedFrame(clientToken(ic), mask, programOf(ic), std::string()),
                           resp)) {
        noteMode(resp);
    } else {
        WIND_DEBUG() << "发送失败: " << bridge_.lastError();
        noteStall();
    }
}

void WindEngine4::reportFocus(FcitxInputContext* ic)
{
    focusUnreported_ = false;
    sendFocusGained(ic);
    sendCaretUpdate(ic);
    sendAndDrain(encodeFrontContextFrame(programOf(ic), std::string(), std::string()));
}

void WindEngine4::sendCaretUpdate(FcitxInputContext* ic)
{
    auto r = caretRectOf(ic);
    if (!r) {
        return; // 宿主没报过光标位置：宁可不报，也别把候选窗钉到屏幕左上角
    }
    sendAndDrain(encodeCaretUpdateFrame(r->x, r->y, r->h));
}

void WindEngine4::noteHostCursor(FcitxInputContext* ic)
{
    int cursor = -1;
    int32_t hostPrev = -1;
    int32_t hostNext = -1;
    if (ic && (ic->contextCaps & CAPACITY_SURROUNDING_TEXT)) {
        char* str = nullptr;
        unsigned int c = 0;
        unsigned int a = 0;
        if (FcitxInstanceGetSurroundingText(instance_, ic, &str, &c, &a) && str) {
            const std::string text(str);
            std::free(str);
            if (a == c) {
                cursor = int(c);
                const size_t len = utf8CharCount(text);
                if (c <= len) {
                    hostPrev = c == 0 ? 0 : codepointAt(text, c - 1);
                    hostNext = c == len ? 0 : codepointAt(text, c);
                }
            }
        }
    }
    const uint16_t before = router_.seqTracker().seqChar();
    router_.seqTracker().noteCursor(cursor, hostPrev, hostNext);
    if (before != router_.seqTracker().seqChar()) {
        WIND_DEBUG() << "光标与预期不符（被挪过）：输入序列记账作废 cursor=" << cursor;
    }
}

uint16_t WindEngine4::prevCharFor(FcitxInputContext* ic)
{
    uint16_t hostChar = 0;
    if (ic && (ic->contextCaps & CAPACITY_SURROUNDING_TEXT)) {
        char* str = nullptr;
        unsigned int c = 0;
        unsigned int a = 0;
        if (FcitxInstanceGetSurroundingText(instance_, ic, &str, &c, &a) && str) {
            const std::string text(str);
            std::free(str);
            if (c > 0 && a == c) {
                const int32_t cp = codepointAt(text, c - 1);
                hostChar = (cp >= 0 && cp < 0x10000) ? uint16_t(cp) : 0;
            }
        }
    }
    return router_.digitTracker().resolve(hostChar);
}

// ── 按键 ────────────────────────────────────────────────────────────

/// fcitx4 的 `FcitxHotkeyGetKey` 会把「keysym 已经表达」的 Shift 从 state 里删掉：
/// Shift+. → keysym '>' 且 state 无 Shift；Shift+字母 → keysym 'A'/'a' 且视状态清掉 Shift。
/// 服务端按 VK + Shift 还原字符（`punct_char`），所以这里按 keysym 反推 Shift。
///
/// 字母与 CapsLock 的组合（XKB 语义）：
///   大写字母 + CapsLock 关 → Shift 按住；大写 + CapsLock 开 → 没有 Shift；
///   小写字母 + CapsLock 开 → Shift 按住（Shift 抵消大写锁）；小写 + CapsLock 关 → 没有 Shift。
bool keysymHasShift(uint32_t sym, bool capsLockOn)
{
    switch (sym) {
    // US 布局的 Shift 上挡符号（与 KeyMap.cpp 的 asciiToVK 表一致）。
    case '!': case '@': case '#': case '$': case '%': case '^': case '&': case '*':
    case '(': case ')': case '_': case '+': case '{': case '}': case '|': case ':':
    case '"': case '<': case '>': case '?': case '~':
    case 0xFE20: // ISO_Left_Tab（Shift+Tab）
        return true;
    default:
        break;
    }
    if (sym >= 'A' && sym <= 'Z') {
        return !capsLockOn;
    }
    if (sym >= 'a' && sym <= 'z') {
        return capsLockOn;
    }
    return false;
}

uint8_t WindEngine4::togglesFor(uint32_t state) const
{
    // fcitx4 的 state 被 FcitxKeyState_SimpleMask 剥掉了 CapsLock 位（XIM 还会显式再减一次），
    // 服务端只能靠每帧的 toggles 判断锁定态：这里把本端跟踪的值补回去。
    uint8_t toggles = statesToToggles(state);
    if (capsLockOn_) {
        toggles = uint8_t(toggles | TOGGLE_CAPSLOCK);
    }
    return toggles;
}

uint32_t WindEngine4::modsFor(uint32_t sym, uint32_t state) const
{
    uint32_t mods = statesToModifiers(state);
    if (keysymHasShift(sym, capsLockOn_)) {
        mods |= KEYMOD_SHIFT;
    }
    return mods;
}

void WindEngine4::sendKeyUp(FcitxInputContext* ic, uint32_t vk, uint32_t mods, uint8_t toggles)
{
    sendCaretUpdate(ic);
    KeyEvent e;
    e.keyCode = vk;
    e.modifiers = mods;
    e.eventType = KEY_EVENT_UP;
    e.toggles = toggles;
    e.eventSeq = ++keySeq_;
    e.prevChar = prevCharFor(ic);
    e.seqChar = router_.seqTracker().seqChar();
    Frame resp;
    if (requestWithRetry(encodeKeyEventFrame(e), resp)) {
        applyResponse(ic, resp, false);
    }
}

bool WindEngine4::sendAltEvent(FcitxInputContext* ic, uint32_t vk, uint8_t eventType, uint32_t mods,
                               uint8_t toggles)
{
    if (!ensureConnected()) {
        return false;
    }
    KeyEvent e;
    e.keyCode = vk;
    e.modifiers = mods;
    e.eventType = eventType;
    e.toggles = toggles;
    e.eventSeq = ++keySeq_;
    e.prevChar = prevCharFor(ic);
    e.seqChar = router_.seqTracker().seqChar();
    Frame resp;
    if (!requestWithRetry(encodeKeyEventFrame(e), resp)) {
        return false;
    }
    const bool consumed = applyResponse(ic, resp, false);
    WIND_DEBUG() << "comment_alt vk=" << hex(vk) << (eventType == KEY_EVENT_DOWN ? " down" : " up")
                 << " mods=" << hex(mods) << (consumed ? " 吃" : " 透传");
    return consumed;
}

void WindEngine4::sendCtrlRaw(FcitxInputContext* ic, uint32_t vk, uint8_t eventType, uint32_t mods,
                              uint8_t toggles)
{
    if (!ensureConnected()) {
        return;
    }
    if (eventType == KEY_EVENT_MOD_UP) {
        sendCaretUpdate(ic);
    }
    KeyEvent e;
    e.keyCode = vk;
    e.modifiers = mods;
    e.eventType = eventType;
    e.toggles = toggles;
    e.eventSeq = ++keySeq_;
    e.prevChar = prevCharFor(ic);
    e.seqChar = router_.seqTracker().seqChar();
    Frame resp;
    if (requestWithRetry(encodeKeyEventFrame(e), resp)) {
        applyResponse(ic, resp, false);
    }
    WIND_DEBUG() << "ctrl_raw vk=" << hex(vk) << (eventType == KEY_EVENT_MOD_DOWN ? " down" : " up")
                 << " mods=" << hex(mods) << " toggles=" << hex(toggles);
}

bool WindEngine4::applyResponse(FcitxInputContext* ic, const Frame& resp, bool hostShortcut)
{
    noteMode(resp);
    ICSink4 sink(instance_, ic);
    return router_.apply(resp, &sink, hostShortcut);
}

void WindEngine4::noteMode(const Frame& frame)
{
    if (mode_.update(frame)) {
        subMode_ = mode_.subModeName();
    }
}

INPUT_RETURN_VALUE WindEngine4::doInput(FcitxKeySym symIn, unsigned int state)
{
    FcitxInputContext* ic = FcitxInstanceGetCurrentIC(instance_);
    if (!ic) {
        return IRV_TO_PROCESS;
    }
    if (!active_ || currentIC_ != ic) {
        attach(ic, false);
    }
    const uint32_t sym = uint32_t(symIn);
    const int code = -int(sym); // fcitx4 的 DoInput 不给硬件键码

    if (sym == 0xFFE5u) { // Caps_Lock
        // 按下不转发给服务：锁定态由系统维护，松开时查 X 服务器再报一帧 keyup。
        // 同时作废 Shift 单击（按住 Shift 时按 CapsLock 的那次 Shift 不算单击）。
        tap_.onPress(sym, nowMs());
        ctrlRaw_.onPress(sym, false);
        return IRV_TO_PROCESS;
    }

    tap_.onPress(sym, nowMs());
    if (panel_) {
        panel_->noteMenuActivity();
    }
    altHold_.noteKey(sym, state);
    if (uint32_t rvk = ctrlRaw_.onPress(sym, altInputSession(router_.hasComposition(), candidatesVisible_));
        rvk != 0) {
        sendCtrlRaw(ic, rvk, KEY_EVENT_MOD_DOWN, modsFor(sym, state) | KEYMOD_CTRL,
                    togglesFor(state));
        return IRV_TO_PROCESS;
    }
    if (uint32_t avk = altVK(sym); avk != 0) {
        const bool forward = altInputSession(router_.hasComposition(), candidatesVisible_);
        altHold_.onAltDown(forward);
        if (forward
            && sendAltEvent(ic, avk, KEY_EVENT_DOWN, modsFor(sym, state) | altHold_.stickyMods(),
                            togglesFor(state))) {
            return IRV_DO_NOTHING;
        }
        return IRV_TO_PROCESS;
    }
    if (isModifierSym(sym)) {
        return IRV_TO_PROCESS;
    }
    const uint32_t vk = keysymToVK(sym);
    const uint32_t mods = modsFor(sym, state);
    if (vk == 0) {
        router_.noteKeyPassthrough(0, mods);
        return IRV_TO_PROCESS;
    }
    if (!ensureConnected()) {
        router_.noteKeyPassthrough(vk, mods);
        return IRV_TO_PROCESS;
    }

    // 密码框实时跟随：同一 IC 内能力位翻转不触发 focus 事件，按键前补报。
    const bool secure = inputScopeMask(ic) != 0;
    if (secure != lastReportedSecure_) {
        sendFocusGained(ic);
    }
    if (!router_.hasComposition()) {
        sendCaretUpdate(ic);
        noteHostCursor(ic);
    }

    KeyEvent e;
    e.keyCode = vk;
    e.modifiers = mods;
    e.eventType = KEY_EVENT_DOWN;
    e.toggles = togglesFor(state);
    e.eventSeq = ++keySeq_;
    e.prevChar = prevCharFor(ic);
    e.seqChar = router_.seqTracker().seqChar();
    Frame resp;
    if (!requestWithRetry(encodeKeyEventFrame(e), resp)) {
        router_.noteKeyPassthrough(vk, mods);
        return IRV_TO_PROCESS;
    }
    const bool consumed = applyResponse(ic, resp, isHostShortcut(state));
    WIND_DEBUG() << "key vk=" << hex(vk) << " mods=" << hex(mods) << " resp=" << hex(resp.cmd)
                 << (consumed ? " 吃" : " 透传");
    if (router_.hasComposition()) {
        sendCaretUpdate(ic);
    }
    if (consumed) {
        eatenKeys_.insert(code);
        return IRV_DO_NOTHING;
    }
    router_.noteKeyPassthrough(vk, mods);
    return IRV_TO_PROCESS;
}

INPUT_RETURN_VALUE WindEngine4::doRelease(FcitxKeySym symIn, unsigned int state)
{
    FcitxInputContext* ic = FcitxInstanceGetCurrentIC(instance_);
    if (!ic) {
        return IRV_TO_PROCESS;
    }
    const uint32_t sym = uint32_t(symIn);
    const int code = -int(sym);

    if (sym == 0xFFE5u) {
        // 松开时 X 服务器的锁定态已经更新：查一次权威值；查不到就按本端状态翻转。
        if (auto on = panel_ ? panel_->capsLockOn() : std::optional<bool>{}) {
            capsLockOn_ = *on;
        } else {
            capsLockOn_ = !capsLockOn_;
        }
        if (mode_.noteCapsLock(capsLockOn_)) {
            subMode_ = mode_.subModeName();
        }
        if (ensureConnected()) {
            sendKeyUp(ic, keysymToVK(sym), modsFor(sym, state), togglesFor(state));
        }
        return IRV_TO_PROCESS;
    }

    // 组字中的 Alt 松开。
    if (uint32_t avk = altVK(sym); avk != 0) {
        const bool forward =
            altHold_.shouldForwardUp(altInputSession(router_.hasComposition(), candidatesVisible_));
        const uint32_t mods = modsFor(sym, state) | altHold_.stickyMods();
        altHold_.reset();
        if (forward && sendAltEvent(ic, avk, KEY_EVENT_UP, mods, togglesFor(state))) {
            return IRV_DO_NOTHING;
        }
        return IRV_TO_PROCESS;
    }
    bool notTap = false;
    if (uint32_t rvk = ctrlRaw_.onRelease(sym, &notTap); rvk != 0) {
        tap_.onRelease(sym, nowMs());
        const uint8_t toggles = togglesFor(state) | (notTap ? TOGGLE_MOD_NOT_TAP : 0);
        sendCtrlRaw(ic, rvk, KEY_EVENT_MOD_UP, modsFor(sym, state) | KEYMOD_CTRL, toggles);
        return IRV_TO_PROCESS;
    }
    if (uint32_t vk = tap_.onRelease(sym, nowMs()); vk != 0) {
        if (ensureConnected()) {
            sendKeyUp(ic, vk, 0, togglesFor(state));
        }
        return IRV_TO_PROCESS;
    }
    if (eatenKeys_.erase(code) > 0) {
        if (uint32_t fvk = keysymToVK(sym); fvk >= 0x70 && fvk <= 0x7B && ensureConnected()) {
            sendKeyUp(ic, fvk, modsFor(sym, state), togglesFor(state));
        }
        return IRV_DO_NOTHING;
    }
    return IRV_TO_PROCESS;
}

// ── push ────────────────────────────────────────────────────────────

void WindEngine4::onPushFrame(Frame frame)
{
    switch (frame.cmd) {
    case CMD_SERVICE_READY:
        shm_.close();
        for (auto& r : overlayShm_) {
            r.close();
        }
        for (auto& r : menuShm_) {
            r.close();
        }
        for (auto& r : panelShm_) {
            r.close();
        }
        hideCandidate();
        if (panel_) {
            panel_->hideAllOverlays();
            panel_->setSkin(CandidateSkin{});
        }
        hidePanels();
        releaseHeldKeys("service_ready");
        {
            const bool hadMenu = panel_ && panel_->menuOpen();
            if (panel_) {
                panel_->closeMenu(nullptr);
            }
            if (serviceSeenOnce_) {
                WIND_INFO() << "服务已重启，重建连接";
                bridge_.close();
                if (FcitxInputContext* ic = focusedIC(); ic && reconnect()) {
                    sendFocusGained(ic);
                }
            }
            if (fullscreen_.resyncAfterRestart()) {
                sendAndDrain(encodeHostFullscreenFrame(true));
            }
            if (hadMenu) {
                sendAndDrain(encodeMenuDismissFrame("service_ready"));
            }
        }
        serviceSeenOnce_ = true;
        break;
    case CMD_STATE_PUSH:
    case CMD_ACTIVATION_STATUS_PUSH:
    case CMD_MODE_PUSH:
        noteMode(frame);
        break;
    case CMD_COMMIT_TEXT:
    case CMD_UPDATE_COMPOSITION:
    case CMD_CLEAR_COMPOSITION:
    case CMD_KEY_TYPE:
        if (FcitxInputContext* ic = focusedIC()) {
            applyResponse(ic, frame, false);
        } else {
            WIND_DEBUG() << "push cmd=" << hex(frame.cmd) << " 无焦点 IC，丢弃";
        }
        break;
    case CMD_KEY_TAP:
    case CMD_KEY_SEQ:
    case CMD_KEY_HOLD:
    case CMD_KEY_RELEASE:
        onKeySynth(frame);
        break;
    case CMD_HOST_RENDER_FRAME:
        if (auto p = decodeHostRenderFrame(frame.payload)) {
            onRenderFrame(*p);
        }
        break;
    case CMD_CANDIDATE_RECTS:
        if (auto rects = decodeCandidateRects(frame.payload); rects && panel_) {
            panel_->setRects(std::move(*rects));
        }
        break;
    case CMD_CANDIDATE_SKIN:
        if (auto skin = decodeCandidateSkin(frame.payload); skin && panel_) {
            panel_->setSkin(std::move(*skin));
        }
        break;
    case CMD_OVERLAY_FRAME:
        if (auto p = decodeOverlayFrame(frame.payload)) {
            onOverlayFrame(*p);
        }
        break;
    case CMD_EXT:
        if (auto ext = decodeExt(frame.payload)) {
            onExt(*ext);
        }
        break;
    default:
        break;
    }
}

void WindEngine4::onRenderFrame(const HostRenderFramePayload& p)
{
    if (!p.visible() || p.width == 0 || p.height == 0) {
        hideCandidate();
        return;
    }
    candidatesVisible_ = true;
    if (!shm_.isOpen() && !shm_.open(shmName())) {
        WIND_WARN() << "打不开候选窗共享内存 " << shmName() << "：" << shm_.lastError();
        return;
    }
    SharedFrame f;
    if (!shm_.snapshot(f) || f.bgra.empty()) {
        WIND_DEBUG() << "候选帧 seq=" << p.seq << " 读取失败或为空";
        return;
    }
    if (panel_) {
        panel_->show(f, f.screenX, f.screenY, (p.flags & FRAME_FLAG_ABSOLUTE_POS) != 0,
                     animInPlace(p.flags, p.seq, f.sequence), frameContentOffset(p, f.sequence));
    }
}

void WindEngine4::hideCandidate()
{
    candidatesVisible_ = false;
    if (panel_) {
        panel_->hide();
    }
}

void WindEngine4::hidePanels()
{
    if (!panel_) {
        return;
    }
    for (uint32_t kind : {OVERLAY_KIND_TOOLBAR, OVERLAY_KIND_SOFTKEYBOARD}) {
        panel_->hideOverlay(kind);
    }
}

void WindEngine4::onOverlayFrame(const OverlayFramePayload& p)
{
    if (int level = menuLevelOfKind(p.kind); level >= 0) {
        onMenuFrame(uint32_t(level), p);
        return;
    }
    const bool isPanel = isPanelOverlayKind(p.kind);
    if (!isPanel && (p.kind < 1 || p.kind > 3)) {
        WIND_DEBUG() << "未知浮层 kind=" << p.kind << "，忽略";
        return;
    }
    if (!panel_) {
        return;
    }
    if (!p.visible() || p.width == 0 || p.height == 0) {
        panel_->hideOverlay(p.kind);
        return;
    }
    static_assert(OVERLAY_KIND_SOFTKEYBOARD == OVERLAY_KIND_TOOLBAR + 1, "panelShm_ 按 kind 连续编址");
    ShmFrameReader& shm = isPanel ? panelShm_[p.kind - OVERLAY_KIND_TOOLBAR] : overlayShm_[p.kind - 1];
    const std::string name = overlayShmName(p.kind);
    if (!shm.isOpen() && !shm.open(name)) {
        WIND_WARN() << "打不开浮层共享内存 " << name << "：" << shm.lastError();
        return;
    }
    SharedFrame f;
    if (!shm.snapshot(f) || f.bgra.empty()) {
        WIND_DEBUG() << "浮层 kind=" << p.kind << " seq=" << p.seq << " 读取失败或为空";
        return;
    }
    panel_->showOverlay(p.kind, f, p);
}

void WindEngine4::onMenuFrame(uint32_t level, const OverlayFramePayload& p)
{
    if (!panel_) {
        return;
    }
    if (!p.visible() || p.width == 0 || p.height == 0) {
        panel_->hideMenuLevel(level);
        return;
    }
    ShmFrameReader& shm = menuShm_[level];
    const std::string name = overlayShmName(p.kind);
    if (!shm.isOpen() && !shm.open(name)) {
        WIND_WARN() << "打不开菜单共享内存 " << name << "：" << shm.lastError();
        return;
    }
    SharedFrame f;
    if (!shm.snapshot(f) || f.bgra.empty()) {
        WIND_DEBUG() << "菜单第 " << level << " 级 seq=" << p.seq << " 读取失败或为空";
        return;
    }
    panel_->showMenuLevel(level, f, p);
}

void WindEngine4::onExt(const ExtEnvelope& ext)
{
    if (ext.kind == EXT_KIND_POS_STATUS_TIP_QUERY) {
        if (panel_) {
            if (auto at = panel_->statusContentOrigin()) {
                sendAndDrain(encodePosFrame(EXT_KIND_POS_STATUS_TIP, at->first, at->second));
            }
        }
        return;
    }
    if (ext.kind == EXT_KIND_POS_CANDIDATE_QUERY) {
        if (panel_) {
            if (auto at = panel_->candidateContentOrigin()) {
                sendAndDrain(encodePosFrame(EXT_KIND_POS_CANDIDATE, at->first, at->second));
            }
        }
        return;
    }
    if (ext.kind != EXT_KIND_SETTINGS_OPEN) {
        WIND_DEBUG() << "未处理的扩展信封 kind=" << ext.kind;
        return;
    }
    auto args = parseSettingsOpenArgs(std::string(ext.body.begin(), ext.body.end()));
    if (!args) {
        WIND_WARN() << "settings.open 的参数解析失败，不启动设置程序";
        return;
    }
    launchSettings(*args);
}

void WindEngine4::launchSettings(const std::vector<std::string>& args)
{
    std::vector<std::string> argv = settingsArgv(args);
    if (access(argv[0].c_str(), X_OK) != 0) {
        WIND_WARN() << "设置程序不存在或不可执行：" << argv[0];
        return;
    }
    WIND_INFO() << "启动设置程序 " << argv[0] << "（" << args.size() << " 个参数）";
    const std::string dir = argv[0].substr(0, argv[0].find_last_of('/') + 1);
    if (!spawnDetached(argv, dir.empty() ? "/" : dir)) {
        WIND_WARN() << "设置程序启动失败：" << argv[0];
    }
}

void WindEngine4::requestMenu(int32_t target, int32_t x, int32_t y, int32_t lx, int32_t ly)
{
    if (!panel_) {
        return;
    }
    Rect wa = panel_->screenWorkArea().value_or(Rect{});
    WIND_DEBUG() << "请求打开菜单 target=" << target << " @(" << x << "," << y << ")";
    sendAndDrain(target == MENU_TARGET_TOOLTIP
                     ? encodeMenuOpenFrame(target, x, y, wa.x, wa.y, wa.x + wa.w, wa.y + wa.h, lx, ly)
                     : encodeMenuOpenFrame(target, x, y, wa.x, wa.y, wa.x + wa.w, wa.y + wa.h));
}

void WindEngine4::reportPanelPos(uint32_t kind, int32_t x, int32_t y, const std::string& output)
{
    const char* ext = panelPosKind(kind);
    if (!ext) {
        WIND_WARN() << "reportPanelPos：kind=" << kind << " 不是工具栏 / 软键盘";
        return;
    }
    sendAndDrain(encodePosFrame(ext, x, y, output));
}

void WindEngine4::reportFullscreen(bool fullscreen)
{
    if (!fullscreen_.change(fullscreen)) {
        return;
    }
    sendAndDrain(encodeHostFullscreenFrame(fullscreen));
}

// ── 命令直通车按键合成 ────────────────────────────────────────────────

namespace {

std::string describeCombo(const KeyComboPayload& c)
{
    std::string s;
    for (const auto& m : c.mods) {
        s += m + "+";
    }
    return s + c.key;
}

} // namespace

void WindEngine4::onKeySynth(const Frame& frame)
{
    FcitxInputContext* ic = focusedIC();
    if (!ic) {
        WIND_DEBUG() << "按键合成 cmd=" << hex(frame.cmd) << " 无焦点 IC，丢弃";
        return;
    }
    if (!keyHolds_.empty() && keyHoldIC_ != ic) {
        releaseHeldKeys("ic_changed");
    }
    const uint64_t now = nowMs();

    if (frame.cmd == CMD_KEY_SEQ) {
        auto seq = decodeKeySeq(frame.payload);
        if (!seq) {
            WIND_WARN() << "key.seq 帧解不开，丢弃";
            return;
        }
        if (seq->size() > kMaxSeqCombos) {
            WIND_WARN() << "key.seq 有 " << seq->size() << " 个组合，超过上限，整条丢弃";
            return;
        }
        std::vector<SynthKey> events;
        for (const auto& c : *seq) {
            auto r = resolveCombo(c);
            if (!r) {
                WIND_WARN() << "key.seq 里有不认识的组合 " << describeCombo(c) << "，整条丢弃";
                return;
            }
            auto t = tapEvents(*r, keyHolds_.heldStates());
            events.insert(events.end(), t.begin(), t.end());
        }
        if (!synthRate_.allow(events.size(), now)) {
            WIND_WARN() << "按键合成过于频繁，丢弃 key.seq";
            return;
        }
        forwardKeys(ic, events);
        return;
    }

    auto payload = decodeKeyCombo(frame.payload);
    if (!payload) {
        WIND_WARN() << "按键合成帧 cmd=" << hex(frame.cmd) << " 解不开，丢弃";
        return;
    }
    auto combo = resolveCombo(*payload);
    if (!combo) {
        WIND_WARN() << "按键合成：不认识的组合 " << describeCombo(*payload) << "，丢弃";
        return;
    }
    switch (frame.cmd) {
    case CMD_KEY_TAP: {
        auto events = tapEvents(*combo, keyHolds_.heldStates());
        if (!synthRate_.allow(events.size(), now)) {
            WIND_WARN() << "按键合成过于频繁，丢弃 key.tap " << describeCombo(*payload);
            return;
        }
        forwardKeys(ic, events);
        break;
    }
    case CMD_KEY_HOLD: {
        if (!synthRate_.allow(combo->mods.size() + 1, now)) {
            WIND_WARN() << "按键合成过于频繁，丢弃 key.hold " << describeCombo(*payload);
            return;
        }
        std::vector<SynthKey> down;
        switch (keyHolds_.hold(*combo, now, keyHoldTimeoutMs_, down)) {
        case KeyHoldTracker::HoldResult::AlreadyHeld:
            WIND_DEBUG() << "key.hold " << describeCombo(*payload) << " 已按住，不重复按";
            return;
        case KeyHoldTracker::HoldResult::Full:
            WIND_WARN() << "已按住的组合已达上限，拒绝 key.hold " << describeCombo(*payload);
            return;
        case KeyHoldTracker::HoldResult::Pressed:
            break;
        }
        keyHoldIC_ = ic;
        forwardKeys(ic, down);
        armKeyHoldExpiry();
        break;
    }
    case CMD_KEY_RELEASE: {
        auto up = keyHolds_.release(*combo);
        if (up.empty()) {
            WIND_DEBUG() << "key.release " << describeCombo(*payload) << "：没有按住，忽略";
            return;
        }
        forwardKeys(ic, up);
        if (keyHolds_.empty()) {
            keyHoldIC_ = nullptr;
        }
        break;
    }
    default:
        break;
    }
}

void WindEngine4::forwardKeys(FcitxInputContext* ic, const std::vector<SynthKey>& keys)
{
    if (!ic) {
        return;
    }
    for (const auto& k : keys) {
        FcitxInstanceForwardKey(instance_, ic, k.release ? FCITX_RELEASE_KEY : FCITX_PRESS_KEY,
                                static_cast<FcitxKeySym>(k.sym), k.states);
    }
    WIND_DEBUG() << "按键合成 " << keys.size() << " 个事件 → " << programOf(ic);
}

void WindEngine4::releaseHeldKeys(const char* reason)
{
    if (keyHolds_.empty()) {
        return;
    }
    std::vector<SynthKey> up = keyHolds_.releaseAll();
    // fcitx4 的 IC 由核心销毁，失焦钩子之外的销毁路径可能留下悬空指针：只往仍是当前 IC
    // 的那个补发抬起（正常路径下失焦钩子已先清理过）。
    if (keyHoldIC_ && keyHoldIC_ == FcitxInstanceGetCurrentIC(instance_)) {
        WIND_INFO() << "补发 key.hold 按住键的抬起（" << reason << "）";
        forwardKeys(keyHoldIC_, up);
    } else {
        WIND_INFO() << "key.hold 按住的键所在 IC 已销毁或已失焦，无处补发抬起（" << reason << "）";
    }
    keyHoldIC_ = nullptr;
    if (keyHoldExpiry_) {
        keyHoldExpiry_->setEnabled(false);
    }
}

void WindEngine4::armKeyHoldExpiry()
{
    std::optional<uint64_t> deadline = keyHolds_.nextDeadline();
    if (!deadline) {
        if (keyHoldExpiry_) {
            keyHoldExpiry_->setEnabled(false);
        }
        return;
    }
    const uint64_t now = nowMs();
    const uint64_t at = fcitx::now(CLOCK_MONOTONIC) + (*deadline > now ? *deadline - now : 0) * 1000;
    if (keyHoldExpiry_) {
        keyHoldExpiry_->setTime(at);
        keyHoldExpiry_->setOneShot();
        return;
    }
    keyHoldExpiry_ = loop_.addTimeEvent(CLOCK_MONOTONIC, at, 0,
                                        [this](fcitx::EventSourceTime*, uint64_t) {
        std::vector<SynthKey> up = keyHolds_.releaseExpired(nowMs());
        if (!up.empty() && keyHoldIC_) {
            WIND_INFO() << "key.hold 超过最长保持时间 " << keyHoldTimeoutMs_ << "ms，自动抬起";
            forwardKeys(keyHoldIC_, up);
        }
        if (keyHolds_.empty()) {
            keyHoldIC_ = nullptr;
        }
        armKeyHoldExpiry();
        return true;
    });
}

} // namespace windlinux
