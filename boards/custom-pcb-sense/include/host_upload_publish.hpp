#pragma once

#include <string>

namespace HostUploadPublish {
constexpr const char* kJournal = "host_replace.part";
constexpr const char* kJournalTemp = "host_replace_new.part";
constexpr const char* kBackup = "host_backup.part";

enum class FileState { Missing, Present, Error };

template <typename Ops>
bool recover(Ops& ops)
{
    const auto journal = ops.fileState(kJournal);
    const auto backup = ops.fileState(kBackup);
    if (journal == FileState::Error || backup == FileState::Error) return false;
    if (journal == FileState::Missing) return backup == FileState::Missing;
    if (backup == FileState::Missing) return ops.remove(kJournal);

    std::string destination;
    if (!ops.readJournal(destination)) return false;
    const auto state = ops.fileState(destination);
    if (state == FileState::Error) return false;
    const bool resolved = state == FileState::Present
        ? ops.remove(kBackup)
        : ops.rename(kBackup, destination);
    return resolved && ops.remove(kJournal);
}

template <typename Ops>
bool publish(Ops& ops, const std::string& partial, const std::string& destination,
             bool replace_existing = true)
{
    if (!recover(ops)) return false;
    const auto state = ops.fileState(destination);
    if (state == FileState::Error) return false;
    if (state == FileState::Missing) return ops.rename(partial, destination);
    if (!replace_existing) return false;
    if (!ops.writeJournal(destination)) return false;
    if (!ops.rename(destination, kBackup)) {
        ops.remove(kJournal);
        return false;
    }
    if (!ops.rename(partial, destination)) {
        // Keep the journal and backup if rollback fails.
        recover(ops);
        return false;
    }
    // The new file is committed. A later recovery can retire the backup.
    recover(ops);
    return true;
}
}  // namespace HostUploadPublish
