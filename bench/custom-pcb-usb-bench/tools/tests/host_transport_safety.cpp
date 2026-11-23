// Standalone tests live outside test/ to avoid adding a second main() to
// PlatformIO's shared test sources.
#include <cassert>
#include <cstdio>
#include <map>
#include <string>

#include "bluetooth_tx_flow.hpp"
#include "host_transfer_guard.hpp"
#include "host_upload_publish.hpp"

struct FakeFiles {
    std::map<std::string, std::string> files;
    int renames = 0;
    int fail_rename = 0;
    bool fail_journal = false;
    bool fail_rollback = false;
    bool corrupt_journal = false;

    bool exists(const std::string& name) { return files.count(name) != 0; }
    bool remove(const std::string& name) { files.erase(name); return true; }
    bool rename(const std::string& from, const std::string& to)
    {
        ++renames;
        if (renames == fail_rename || (fail_rollback && from == HostUploadPublish::kBackup) ||
            !exists(from) || exists(to)) return false;
        files[to] = files[from];
        files.erase(from);
        return true;
    }
    bool writeJournal(const std::string& name)
    {
        if (fail_journal) return false;
        files[HostUploadPublish::kJournal] = name;
        return true;
    }
    bool readJournal(std::string& name)
    {
        if (corrupt_journal) return false;
        name = files[HostUploadPublish::kJournal];
        return true;
    }
};

static FakeFiles replacement()
{
    FakeFiles fs;
    fs.files["report.bin"] = "OLD";
    fs.files["host_upload.part"] = "NEW_VERIFIED";
    return fs;
}

static void test_replacement_success_and_failure_keep_verified_contents()
{
    for (int fail_at = 0; fail_at <= 2; ++fail_at) {
        FakeFiles fs = replacement();
        fs.fail_rename = fail_at;
        const bool success = HostUploadPublish::publish(fs, "host_upload.part", "report.bin");
        assert(success == (fail_at == 0));
        assert(fs.files["report.bin"] == (success ? "NEW_VERIFIED" : "OLD"));
        assert(!fs.exists(HostUploadPublish::kBackup));
    }
    FakeFiles fs = replacement();
    fs.fail_journal = true;
    assert(!HostUploadPublish::publish(fs, "host_upload.part", "report.bin"));
    assert(fs.files["report.bin"] == "OLD");
}

static void test_failed_rollback_retains_backup_for_recovery()
{
    FakeFiles fs = replacement();
    fs.fail_rename = 2;
    fs.fail_rollback = true;
    assert(!HostUploadPublish::publish(fs, "host_upload.part", "report.bin"));
    assert(fs.files[HostUploadPublish::kBackup] == "OLD");
    assert(fs.exists(HostUploadPublish::kJournal));
    fs.fail_rollback = false;
    assert(HostUploadPublish::recover(fs));
    assert(fs.files["report.bin"] == "OLD");
}

static void test_recovery_at_each_interrupted_publication_step()
{
    for (int step = 0; step < 3; ++step) {
        FakeFiles fs = replacement();
        assert(fs.writeJournal("report.bin"));
        if (step >= 1) assert(fs.rename("report.bin", HostUploadPublish::kBackup));
        if (step >= 2) assert(fs.rename("host_upload.part", "report.bin"));
        assert(HostUploadPublish::recover(fs));
        assert(fs.files["report.bin"] == (step == 2 ? "NEW_VERIFIED" : "OLD"));
        assert(!fs.exists(HostUploadPublish::kBackup));
        assert(!fs.exists(HostUploadPublish::kJournal));
    }
}

static void test_bad_journal_and_orphaned_backup_are_preserved()
{
    FakeFiles fs;
    fs.files[HostUploadPublish::kBackup] = "OLD";
    assert(!HostUploadPublish::recover(fs));
    assert(fs.files[HostUploadPublish::kBackup] == "OLD");
    fs.files[HostUploadPublish::kJournal] = "corrupt";
    fs.corrupt_journal = true;
    assert(!HostUploadPublish::recover(fs));
    assert(fs.files[HostUploadPublish::kBackup] == "OLD");
}

static void test_bluetooth_flow_waits_for_completion_and_congestion_clear()
{
    BluetoothTx::Flow flow;
    flow.reset(12, 1);
    assert(flow.begin(12, 1));
    assert(!flow.begin(12, 1));
    flow.complete(12, true, true);
    assert(!flow.pending());
    assert(!flow.begin(12, 1));
    flow.congestion(12, false);
    assert(flow.begin(12, 1));
    flow.complete(12, false, false);
    assert(flow.failed());
    assert(!flow.begin(12, 1));
}

static void test_bluetooth_flow_ignores_old_handle_and_epoch()
{
    BluetoothTx::Flow flow;
    flow.reset(12, 1);
    assert(flow.begin(12, 1));
    flow.reset(13, 2);
    assert(!flow.matches(12, 1));
    flow.complete(12, false, true);
    flow.congestion(12, true);
    assert(flow.begin(13, 2));
    flow.reset(0, 3);
    assert(!flow.begin(13, 2));
}

static void test_session_disconnect_timeout_and_transport_ownership()
{
    using namespace HostTransferGuard;
    assert(alive(true, 5, 100, 110, true, 5, 60));
    assert(!alive(true, 5, 100, 110, false, 5, 60));
    assert(!alive(true, 5, 100, 110, true, 6, 60));
    assert(!alive(true, 5, 100, 160, true, 5, 60));
    assert(alive(false, 0, 100, 110, false, 99, 60));
    assert(owns(3, 5, 3, 5));
    assert(!owns(3, 5, 3, 6));
    assert(!owns(3, 5, 0, 0));
}

static void test_local_echo_requires_devboard_test_mode_and_bounded_ascii()
{
    using HostTransferGuard::buildLocalEcho;
    std::string response;
    assert(buildLocalEcho(true, true, true, "hello  ESP!", 154, response));
    assert(response == "ECHO TEXT: hello  ESP!");
    assert(buildLocalEcho(true, true, true, std::string(154, 'x'), 154, response));
    assert(!buildLocalEcho(false, true, true, "hello", 154, response));
    assert(response.empty());
    assert(!buildLocalEcho(true, false, true, "hello", 154, response));
    assert(!buildLocalEcho(true, true, false, "hello", 154, response));
    assert(!buildLocalEcho(true, true, true, "", 154, response));
    assert(!buildLocalEcho(true, true, true, std::string(155, 'x'), 154, response));
    assert(!buildLocalEcho(true, true, true, "hello\nTX", 154, response));
    assert(!buildLocalEcho(true, true, true, std::string("hi\0x", 4), 154, response));
    assert(!buildLocalEcho(true, true, true, "\xFF", 154, response));
}

int main()
{
    test_replacement_success_and_failure_keep_verified_contents();
    test_failed_rollback_retains_backup_for_recovery();
    test_recovery_at_each_interrupted_publication_step();
    test_bad_journal_and_orphaned_backup_are_preserved();
    test_bluetooth_flow_waits_for_completion_and_congestion_clear();
    test_bluetooth_flow_ignores_old_handle_and_epoch();
    test_session_disconnect_timeout_and_transport_ownership();
    test_local_echo_requires_devboard_test_mode_and_bounded_ascii();
    std::puts("8 host transport safety tests passed (including publication fault injection and local echo)");
}
