// CompatEvent 的实现：单线程（fcitx4 主线程）事件泵 + 线程安全的任务投递队列。
//
// 为什么不自己开线程轮询 X：X11Panel 的鼠标回调会经同一条 BridgeClient 发同步请求，
// BridgeClient 明确不是线程安全的；把面板放主线程，与按键串行，最省心。
#include "CompatEvent.h"

#include <poll.h>
#include <time.h>

#include <algorithm>
#include <utility>

namespace fcitx {

struct EventLoop::Impl {
    struct IOEntry {
        EventSourceIO* src = nullptr;
        int fd = -1;
        std::function<void(EventSourceIO*, int, IOEventFlags)> cb;
    };
    struct TimeEntry {
        EventSourceTime* src = nullptr;
        uint64_t interval = 0;
        std::function<bool(EventSourceTime*, uint64_t)> cb;
    };

    uint64_t nextId = 1;
    std::unordered_map<uint64_t, IOEntry> ios;
    std::unordered_map<uint64_t, TimeEntry> timers;

    std::mutex postMu;
    std::deque<std::function<void()>> posted;
};

uint64_t now(clockid_t clock)
{
    struct timespec ts;
    clock_gettime(clock, &ts);
    return uint64_t(ts.tv_sec) * 1000000ull + uint64_t(ts.tv_nsec) / 1000ull;
}

EventLoop::EventLoop() : impl_(std::make_shared<Impl>()) { }

EventLoop::~EventLoop()
{
    // 事件源都应在 EventLoop 之前销毁（成员声明顺序保证）。这里只清掉残留，防止悬空。
    impl_->ios.clear();
    impl_->timers.clear();
}

std::unique_ptr<EventSourceIO> EventLoop::addIOEvent(
    int fd, IOEventFlags flags, std::function<void(EventSourceIO*, int, IOEventFlags)> cb)
{
    (void)flags; // 只关心可读（X11Panel 只用 In）
    const uint64_t id = impl_->nextId++;
    auto src = std::unique_ptr<EventSourceIO>(new EventSourceIO(this, fd, id));
    impl_->ios[id] = Impl::IOEntry{src.get(), fd, std::move(cb)};
    return src;
}

std::unique_ptr<EventSourceTime> EventLoop::addTimeEvent(
    clockid_t clock, uint64_t time, uint64_t interval,
    std::function<bool(EventSourceTime*, uint64_t)> cb)
{
    (void)clock; // 本实现只按 CLOCK_MONOTONIC 记账（X11Panel 与引擎都只用这个）
    const uint64_t id = impl_->nextId++;
    auto src = std::unique_ptr<EventSourceTime>(new EventSourceTime(this, time, interval, id));
    impl_->timers[id] = Impl::TimeEntry{src.get(), interval, std::move(cb)};
    return src;
}

void EventLoop::removeIO(uint64_t id)
{
    impl_->ios.erase(id);
}

void EventLoop::removeTimer(uint64_t id)
{
    impl_->timers.erase(id);
}

EventSourceIO::~EventSourceIO()
{
    if (loop_) {
        loop_->removeIO(id_);
    }
}

EventSourceTime::~EventSourceTime()
{
    if (loop_) {
        loop_->removeTimer(id_);
    }
}

void EventLoop::post(std::function<void()> fn)
{
    std::lock_guard<std::mutex> lock(impl_->postMu);
    impl_->posted.push_back(std::move(fn));
}

void EventLoop::runOnce()
{
    // 1. 推送线程投递的任务（SERVICE_READY、上屏帧…）先做：它们可能改面板 / SHM 状态。
    std::deque<std::function<void()>> tasks;
    {
        std::lock_guard<std::mutex> lock(impl_->postMu);
        tasks.swap(impl_->posted);
    }
    for (auto& t : tasks) {
        t();
    }

    // 2. X 连接可读：非阻塞 poll，回调里 X11Panel 自己 while 到读空。
    std::vector<struct pollfd> fds;
    std::vector<uint64_t> ids;
    fds.reserve(impl_->ios.size());
    ids.reserve(impl_->ios.size());
    for (auto& [id, e] : impl_->ios) {
        if (e.src && e.src->isEnabled()) {
            fds.push_back(pollfd{e.fd, POLLIN, 0});
            ids.push_back(id);
        }
    }
    if (!fds.empty()) {
        const int rc = poll(fds.data(), nfds_t(fds.size()), 0);
        if (rc > 0) {
            for (size_t i = 0; i < fds.size(); ++i) {
                if (fds[i].revents == 0) {
                    continue;
                }
                auto it = impl_->ios.find(ids[i]);
                if (it == impl_->ios.end() || !it->second.src || !it->second.src->isEnabled()) {
                    continue;
                }
                // 拷贝回调：回调里可能增删事件源（X11Panel 的断线处理只禁用自己，但别赌）。
                auto cb = it->second.cb;
                cb(it->second.src, it->second.fd, IOEventFlag::In);
            }
        }
    }

    // 3. 到期计时器。一次性计时器先停用再回调（与 fcitx5 一致：回调里 armed() 看到的是已触发）。
    const uint64_t t = now(CLOCK_MONOTONIC);
    std::vector<uint64_t> due;
    for (auto& [id, e] : impl_->timers) {
        if (e.src && e.src->isEnabled() && e.src->time() <= t) {
            due.push_back(id);
        }
    }
    for (uint64_t id : due) {
        auto it = impl_->timers.find(id);
        if (it == impl_->timers.end() || !it->second.src) {
            continue;
        }
        EventSourceTime* src = it->second.src;
        const uint64_t interval = it->second.interval;
        auto cb = it->second.cb;
        if (interval == 0) {
            src->setEnabled(false);
        }
        const bool keep = cb(src, t);
        // 回调可能销毁了事件源（重新 arm 同一个 unique_ptr）：只按 id 重新查，别碰旧指针。
        auto after = impl_->timers.find(id);
        if (after == impl_->timers.end() || after->second.src != src) {
            continue;
        }
        if (!keep) {
            src->setEnabled(false);
        } else if (interval > 0) {
            src->setTime(t + interval);
        }
    }
}

} // namespace fcitx
