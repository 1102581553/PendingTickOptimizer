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

static std::atomic<int>      gTickBudgetRemaining{0};
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

// ── 工具函数：检查队列是否全是 portal tick ────────────────

static bool isPortalOnlyQueue(BlockTickingQueue::TickDataSet const& queue) {
    bool hasPortal = false;
    for (auto const& blockTick : queue.mC) {
        if (blockTick.mIsRemoved) continue;
        if (!blockTick.mData.mBlock) continue;
        auto name = blockTick.mData.mBlock->getTypeName();
        if (name == "minecraft:portal" ||
            name == "minecraft:end_portal" ||
            name == "minecraft:end_gateway") {
            hasPortal = true;
        } else {
            return false; // 有非 portal 方块，放行
        }
    }
    return hasPortal;
}

// ── Hook ──────────────────────────────────────────────────

LL_TYPE_INSTANCE_HOOK(
    LevelTickHook,
    ll::memory::HookPriority::Normal,
    Level,
    &Level::$tick,
    void
) {
    if (pluginEnabled.load(std::memory_order_relaxed)
        && config.enabled
        && config.budgetEnabled) {
        gTickBudgetRemaining.store(
            std::max(1, config.globalBudgetPerTick),
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
    if (!pluginEnabled.load(std::memory_order_relaxed) || !config.enabled || !config.budgetEnabled) {
        return origin(region, until, max, instaTick_);
    }

    // 非纯 portal 队列直接放行，零开销
    if (!isPortalOnlyQueue(this->mNextTickQueue.mC)) {
        return origin(region, until, max, instaTick_);
    }

    totalCallCount.fetch_add(1, std::memory_order_relaxed);

    // 单次调用限制
    if (max > config.budgetPerTick) {
        max = config.budgetPerTick;
    }

    // 全局预算限制
    int remaining = gTickBudgetRemaining.load(std::memory_order_relaxed);
    if (remaining <= 0) {
        totalCapped.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    int allowed = std::min(max, remaining);
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

    totalQueued.fetch_add(this->mNextTickQueue.mC.size(), std::memory_order_relaxed);

    if (allowed < max) {
        totalCapped.fetch_add(1, std::memory_order_relaxed);
    }

    return origin(region, until, max, allowed);
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
                    "PortalTicks | calls: {} | avg queue: {:.1f} | capped: {} ({:.1f}%)",
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
    std::filesystem::create_directories(getSelf().getConfigDir());
    if (!loadConfig()) {
        logger().warn("Failed to load config, saving defaults");
        saveConfig();
    }
    logger().info(
        "Loaded. budget={}(per={}, global={})",
        config.budgetEnabled,
        config.budgetPerTick,
        config.globalBudgetPerTick
    );
    return true;
}

bool PluginImpl::enable() {
    pluginEnabled.store(true, std::memory_order_relaxed);

    totalCallCount.store(0, std::memory_order_relaxed);
    totalQueued.store(0, std::memory_order_relaxed);
    totalCapped.store(0, std::memory_order_relaxed);
    gTickBudgetRemaining.store(0, std::memory_order_relaxed);

    if (!hookInstalled.load(std::memory_order_relaxed)) {
        LevelTickHook::hook();
        PendingTicksHook::hook();
        hookInstalled.store(true, std::memory_order_relaxed);
        logger().info("Hooks installed");
    }

    startStatsTask();
    logger().info(
        "Enabled. budget={}(per={}, global={})",
        config.budgetEnabled,
        config.budgetPerTick,
        config.globalBudgetPerTick
    );
    return true;
}

bool PluginImpl::disable() {
    pluginEnabled.store(false, std::memory_order_relaxed);

    if (hookInstalled.load(std::memory_order_relaxed)) {
        LevelTickHook::unhook();
        PendingTicksHook::unhook();
        hookInstalled.store(false, std::memory_order_relaxed);
        logger().info("Hooks uninstalled");
    }

    logger().info("Disabled");
    return true;
}

} // namespace pending_tick_optimizer

LL_REGISTER_MOD(pending_tick_optimizer::PluginImpl, pending_tick_optimizer::PluginImpl::getInstance());
