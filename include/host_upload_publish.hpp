#pragma once

#include <string>

// Recoverable SPIFFS replacement: keep the old file until the verified upload
// is published. Ops isolates filesystem calls for fault-injection tests.
namespace HostUploadPublish {
constexpr const char* kJournal = "host_replace.part";
constexpr const char* kBackup = "host_backup.part";

template <typename Ops>
bool recover(Ops& ops)
{
    if (!ops.exists(kJournal)) {
        // Never silently delete an orphaned backup.
        return !ops.exists(kBackup);
    }
    std::string destination;
    if (!ops.readJournal(destination)) {
        return false;
    }
    if (ops.exists(kBackup)) {
        if (ops.exists(destination)) {
            if (!ops.remove(kBackup)) {
                return false;
            }
        } else if (!ops.rename(kBackup, destination)) {
            return false;
        }
    }
    return ops.remove(kJournal);
}

template <typename Ops>
bool publish(Ops& ops, const std::string& partial, const std::string& destination)
{
    if (!recover(ops)) {
        return false;
    }
    if (!ops.exists(destination)) {
        return ops.rename(partial, destination);
    }
    if (!ops.writeJournal(destination)) {
        return false;
    }
    if (!ops.rename(destination, kBackup)) {
        ops.remove(kJournal);
        return false;
    }
    if (!ops.rename(partial, destination)) {
        // Keep the journal/backup if rollback fails; recover() retries at boot.
        recover(ops);
        return false;
    }
    // Publication succeeded. Cleanup failure leaves a recoverable journal.
    recover(ops);
    return true;
}
}  // namespace HostUploadPublish
