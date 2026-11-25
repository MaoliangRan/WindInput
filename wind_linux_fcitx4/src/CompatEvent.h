// fcitx5 事件循环 API 的最小兼容层（只服务复用的 X11 候选窗代码）。
//
// X11Panel 原先是给 Fcitx5 写的，实际只用到这里四个东西：addIOEvent / addTimeEvent /
// EventSource::setEnabled / now()。本机没有 Fcitx5 5.x，故用本文件在 fcitx4 addon 里提供
// 同名同语义的接口，X11Panel 源码只改 include，不改逻辑。
//
// 驱动方式：fcitx4 主线程每 10ms 调一次 EventLoop::runOnce()（见 WindEngine4 的 tick）。
// 所有回调都在 fcitx4 主线程执行，与按键处理同线程——BridgeClient 不是线程安全的，这一点
// 必须保持。推送线程只经 post() 投递任务，不直接碰面板与 InputContext。
#pragma once

#include <cstdint>
#include <ctime>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace fcitx {

using IOEventFlags = unsigned int;

namespace IOEventFlag {
constexpr IOEventFlags In = 1u;
constexpr IOEventFlags Out = 2u;
constexpr IOEventFlags Err = 4u;
constexpr IOEventFlags Hup = 8u;
} // namespace IOEventFlag

class EventSourceIO;
class EventSourceTime;
class EventLoop;

/// 事件源基类：只保存「是否启用」。X11Panel 用 setEnabled(false) 停一次性计时器 / 断开的
/// X 连接 fd，不销毁对象（销毁可能发生在它自己的回调里）。
class EventSource {
public:
    virtual ~EventSource() { }

    void setEnabled(bool enabled) { enabled_ = enabled; }
    bool isEnabled() const { return enabled_; }

protected:
    EventSource() = default;

    bool enabled_ = true;
    EventLoop* loop_ = nullptr;
};

class EventSourceIO : public EventSource {
public:
    ~EventSourceIO() override;

    int fd() const { return fd_; }

private:
    friend class EventLoop;
    EventSourceIO(EventLoop* loop, int fd, uint64_t id) : fd_(fd), id_(id)
    {
        loop_ = loop;
    }
    int fd_;
    uint64_t id_;
};

class EventSourceTime : public EventSource {
public:
    ~EventSourceTime() override;

    /// 到期时刻（CLOCK_MONOTONIC 微秒）。
    uint64_t time() const { return time_; }
    /// 重设到期时刻并重新启用（复用同一个事件源时不必再调 setEnabled）。
    void setTime(uint64_t time)
    {
        time_ = time;
        enabled_ = true;
    }
    /// 改成一次性（回调触发后自动停用）。
    void setOneShot() { interval_ = 0; }
    void setInterval(uint64_t interval) { interval_ = interval; }

private:
    friend class EventLoop;
    EventSourceTime(EventLoop* loop, uint64_t time, uint64_t interval, uint64_t id)
        : time_(time), interval_(interval), id_(id)
    {
        loop_ = loop;
    }
    uint64_t time_ = 0;
    uint64_t interval_ = 0;
    uint64_t id_;
};

class EventLoop {
public:
    EventLoop();
    ~EventLoop();
    EventLoop(const EventLoop&) = delete;
    EventLoop& operator=(const EventLoop&) = delete;

    std::unique_ptr<EventSourceIO> addIOEvent(int fd, IOEventFlags flags,
                                              std::function<void(EventSourceIO*, int, IOEventFlags)> cb);
    std::unique_ptr<EventSourceTime> addTimeEvent(clockid_t clock, uint64_t time, uint64_t interval,
                                                  std::function<bool(EventSourceTime*, uint64_t)> cb);

    /// 线程安全：从推送线程把任务转回主线程（下一次 runOnce 执行）。
    void post(std::function<void()> fn);

    /// fcitx4 主线程每 10ms 调一次：执行 post 任务、轮询 X fd、跑到期计时器。
    void runOnce();

private:
    friend class EventSourceIO;
    friend class EventSourceTime;
    struct Impl;
    std::shared_ptr<Impl> impl_;

    void removeIO(uint64_t id);
    void removeTimer(uint64_t id);
};

/// 单调时钟微秒（与 fcitx5 的 fcitx::now 同量纲）。
uint64_t now(clockid_t clock);

} // namespace fcitx
