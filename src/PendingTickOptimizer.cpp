#include "PendingTickOptimizer.h"
#include "ll/api/memory/Hook.h"
#include "ll/api/mod/RegisterHelper.h"
#include "ll/api/coro/CoroTask.h"
#include "ll/api/thread/ServerThreadExecutor.h"
#include "ll/api/io/Logger.h"
#include "ll/api/io/LoggerRegistry.h"
#include "mc/world/level/BlockSource.h"
#include "mc/world/level/Level.h"
#include "mc/world/level/BlockTickingQueue.h"
#include "mc/world/level/Tick.h"
#include <filesystem>
#include <chrono>
#include <atomic>

namespace pending_tick_optimizer {

static Config                          config;
static std::shared_ptr<ll::io::Logger> log;
static std::atomic<bool>               pluginEnabled{false};
static std::atomic<bool>               hookInstalled{false};

// 全局预算：每 tick 由 LevelTickHook 重置，由 PendingTicksHook 消耗
static std::atomic<int> gTickBudgetRemaining{0};

// 统计
static std::atomic<uint64_t> totalCallCount{0};
static std::atomic<uint64_t> totalQueued{0};
static std::atomic<uint64_t> totalCapped{0};

// ── 工具函数 ──────────────────────────────────────────────

Config& getConfig() { return config; }

bool loadConfig() {
    auto path = PluginImpl::getInstance().getSelf().getConfigDir() / "config.json";
    return ll::config::loadConfig(config, path);
}

bool saveConfig() {
    auto path = PluginImpl::getInstance().getSelf().getConfigDir() / "config.json";
    return ll::config::saveConfig(config, path);
}

ll::io::Logger& logger() {
    if (!log) {
        log = ll::io::LoggerRegistry::getInstance().getOrCreate("PendingTickOptimizer");
    }
    return *log;
}

// ── Hook ──────────────────────────────────────────────────

// Level::tick 在每 tick 最开始执行，用于重置全局预算
LL_TYPE_INSTANCE_HOOK(
    LevelTickHook,
    ll::memory::HookPriority::Normal,
    Level,
    &Level::tick,
    void
) {
    if (pluginEnabled.load(std::memory_order_relaxed)
        && config.enabled
        && config.budgetEnabled) {
        gTickBudgetRemaining.store(
            std::max(1, config.budgetPerTick),
            std::memory_order_relaxed
        );
    }
    return origin();
}

LL_TYPE_INSTANCE_HOOK(
    PendingTicksHook,
    ll::memory::HookPriority::Normal,
    BlockTickingQueue,
    &BlockTickingQueue::tickPendingTicks,
    bool,
    BlockSource& region,
    Tick const&  until,
    int          max,
    bool         instaTick_
) {
    if (!pluginEnabled.load(std::memory_order_relaxed) || !config.enabled) {
        return origin(region, until, max, instaTick_);
    }

    totalCallCount.fetch_add(1, std::memory_order_relaxed);

    if (config.budgetEnabled) {
        // 原子地从全局预算中申请配额
        // fetch_sub 返回旧值，用旧值来判断本次能拿到多少
        int remaining = gTickBudgetRemaining.load(std::memory_order_relaxed);
        if (remaining <= 0) {
            // 预算已耗尽，跳过本次处理
            totalCapped.fetch_add(1, std::memory_order_relaxed);
            return false;
        }

        int allowed = std::min(max, remaining);
        // CAS 循环保证原子扣减正确
        while (!gTickBudgetRemaining.compare_exchange_weak(
            remaining,
            remaining - allowed,
            std::memory_order_relaxed,
            std::memory_order_relaxed
        )) {
            if (remaining <= 0) {
                totalCapped.fetch_add(1, std::memory_order_relaxed);
                return false;
            }
            allowed = std::min(max, remaining);
        }

        auto queueSize = static_cast<uint64_t>(this->mNextTickQueue->mC.size());
        totalQueued.fetch_add(queueSize, std::memory_order_relaxed);

        if (allowed < max) {
            totalCapped.fetch_add(1, std::memory_order_relaxed);
        }
        max = allowed;
    }

    return origin(region, until, max, instaTick_);
}

// ── 统计输出协程 ──────────────────────────────────────────

void startStatsTask() {
    ll::coro::keepThis([]() -> ll::coro::CoroTask<> {
        while (pluginEnabled.load(std::memory_order_relaxed)) {
            int interval = getConfig().statsIntervalSec;
            if (interval < 1) interval = 5;
            co_await std::chrono::seconds(interval);

            if (!pluginEnabled.load(std::memory_order_relaxed)) break;

            if (getConfig().debug) {
                uint64_t calls  = totalCallCount.load(std::memory_order_relaxed);
                uint64_t queued = totalQueued.load(std::memory_order_relaxed);
                uint64_t capped = totalCapped.load(std::memory_order_relaxed);
                float    capPct = calls > 0
                    ? static_cast<float>(capped) / static_cast<float>(calls) * 100.0f
                    : 0.0f;

                logger().info(
                    "PendingTicks | calls: {} | avg queue: {:.1f} | capped: {} ({:.1f}%)",
                    calls,
                    calls > 0 ? static_cast<float>(queued) / static_cast<float>(calls) : 0.0f,
                    capped,
                    capPct
                );

                totalCallCount.store(0, std::memory_order_relaxed);
                totalQueued.store(0, std::memory_order_relaxed);
                totalCapped.store(0, std::memory_order_relaxed);
            }
        }
    }).launch(ll::thread::ServerThreadExecutor::getDefault());
}

// ── 生命周期 ──────────────────────────────────────────────

PluginImpl& PluginImpl::getInstance() {
    static PluginImpl instance;
    return instance;
}

bool PluginImpl::load() {
    std::filesystem::create_directories(getSelf().getConfigDir())_
