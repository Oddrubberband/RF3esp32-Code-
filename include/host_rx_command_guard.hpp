#pragma once

#include <atomic>
#include <string_view>

namespace HostRxCommandGuard {
// Passive observations and explicit cancellation do not reserve/change the
// receive path. All other operator commands are serialized against RF START.
inline bool needsReservation(std::string_view command, std::string_view action)
{
    if (command == "RX" || command == "STOP" || command == "STATUS" || command == "HELLO" ||
        command == "HELP" || command == "?" || command == "FILES" || command == "LS" || command == "ECHO")
        return false;
    if (command == "STORAGE" && action == "LIST") return false;
    if ((command == "UPLOAD" || command == "DOWNLOAD") && action == "CANCEL") return false;
    if (command == "CW" && action == "STOP") return false;
    return true;
}

// A command owns this flag only after checking receiver state while holding the
// radio lock. The RX worker tests it under that same lock before any drain/tick.
// Destruction clears the reservation on every handler return path.
class Reservation {
public:
    explicit Reservation(std::atomic_bool* flag = nullptr) : flag_(flag) {}
    ~Reservation() { if (flag_) flag_->store(false); }
    Reservation(const Reservation&) = delete;
    Reservation& operator=(const Reservation&) = delete;
    void acquired(std::atomic_bool& flag) { flag_ = &flag; }
private:
    std::atomic_bool* flag_ = nullptr;
};
}  // namespace HostRxCommandGuard
