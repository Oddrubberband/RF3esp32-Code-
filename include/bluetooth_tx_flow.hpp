#pragma once

#include <atomic>
#include <cstdint>

namespace BluetoothTx {
// Callback-safe flow state. The TX task is the sole writer of payloads.
class Flow {
public:
    void reset(uint32_t handle, uint32_t epoch)
    {
        handle_.store(0);
        pending_.store(false);
        congested_.store(false);
        failed_.store(false);
        epoch_.store(epoch);
        handle_.store(handle);
    }

    bool matches(uint32_t handle, uint32_t epoch) const
    {
        return handle != 0 && handle_.load() == handle && epoch_.load() == epoch;
    }

    bool begin(uint32_t handle, uint32_t epoch)
    {
        if (!matches(handle, epoch) || pending_.load() || congested_.load() || failed_.load()) {
            return false;
        }
        pending_.store(true);
        // reset() can race a connection check. Only this TX task starts
        // writes, so undo a stale begin without touching another writer.
        if (!matches(handle, epoch)) {
            pending_.store(false);
            return false;
        }
        return true;
    }

    void complete(uint32_t handle, bool success, bool congested)
    {
        if (handle_.load() != handle) return;
        congested_.store(congested);
        failed_.store(!success);
        pending_.store(false);
    }

    void congestion(uint32_t handle, bool congested)
    {
        if (handle_.load() == handle) congested_.store(congested);
    }

    bool pending() const { return pending_.load(); }
    bool congested() const { return congested_.load(); }
    bool failed() const { return failed_.load(); }

private:
    std::atomic<uint32_t> handle_{0};
    std::atomic<uint32_t> epoch_{0};
    std::atomic_bool pending_{false};
    std::atomic_bool congested_{false};
    std::atomic_bool failed_{false};
};
}  // namespace BluetoothTx
