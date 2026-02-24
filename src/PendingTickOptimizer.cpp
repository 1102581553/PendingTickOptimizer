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

// 统计
static std::atomic<uint64_t> totalCallCount{0};   // tickPendingTicks 调用次数
static std::atomic<uint64_t> totalQueued{0};       // 累计队列中的计划刻数
static std::atomic<uint64_t> totalCapped{0};       // 被 budget 限制的次数（max 被压低）

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
        // 统计队列大小（调用前）
        auto queueSize = static_cast<uint64_t>(this->mNextTickQueue->mC.size());
        totalQueued.fetch_add(queueSize, std::memory_order_relaxed);

        if (max > config.budgetPerTick) {
            totalCapped.fetch_add(1, std::memory_order_relaxed);
            max = config.budgetPerTick;
        }
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
                uint64_t calls   = totalCallCount.load(std::memory_order_relaxed);
                uint64_t queued  = totalQueued.load(std::memory_order_relaxed);
                uint64_t capped  = totalCapped.load(std::memory_order_relaxed);
                float    capPct  = calls > 0
                    ? static_cast<float>(capped) / static_cast<float>(calls) * 100.0f
                    : 0.0f;

                logger().info(
                    "PendingTicks | calls: {} | avg queue: {:.1f} | capped: {} ({:.1f}%)",
                    calls,
                    calls > 0 ? static_cast<float>(queued) / static_cast<float>(calls) : 0.0f,
                    capped,
                    capPct
                );

                // 重置统计
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
    std::filesystem::create_directories(getSelf().getConfigDir());
    if (!loadConfig()) {
        logger().warn("Failed to load config, saving defaults");
        saveConfig();
    }
    logger().info("Loaded. budget={}({})", config.budgetEnabled, config.budgetPerTick);
    return true;
}

bool PluginImpl::enable() {
    pluginEnabled.store(true, std::memory_order_relaxed);

    totalCallCount.store(0, std::memory_order_relaxed);
    totalQueued.store(0, std::memory_order_relaxed);
    totalCapped.store(0, std::memory_order_relaxed);

    if (!hookInstalled.load(std::memory_order_relaxed)) {
        PendingTicksHook::hook();
        hookInstalled.store(true, std::memory_order_relaxed);
        logger().info("Hook installed on BlockTickingQueue::tickPendingTicks");
    }

    startStatsTask();
    logger().info("Enabled. budget={}({})", config.budgetEnabled, config.budgetPerTick);
    return true;
}

bool PluginImpl::disable() {
    pluginEnabled.store(false, std::memory_order_relaxed);

    if (hookInstalled.load(std::memory_order_relaxed)) {
        PendingTicksHook::unhook();
        hookInstalled.store(false, std::memory_order_relaxed);
        logger().info("Hook uninstalled");
    }

    logger().info("Disabled");
    return true;
}

} // namespace pending_tick_optimizer

LL_REGISTER_MOD(pending_tick_optimizer::PluginImpl, pending_tick_optimizer::PluginImpl::getInstance());
