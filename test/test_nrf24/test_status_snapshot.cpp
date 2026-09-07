#include <atomic>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>

#include <unity.h>

#include "app_status.hpp"

namespace {

void test_status_snapshot_owns_filename_after_selection_changes()
{
    std::mutex mutex;
    AppStatus::SnapshotCache<std::mutex> cache(mutex);
    std::string selected = "original payload.bin";
    cache.selectFile(selected, 123);
    const auto captured = cache.capture();
    selected.assign(1024, 'x');
    cache.selectFile(selected, 456);

    TEST_ASSERT_EQUAL_STRING("original payload.bin", captured.selected_name.c_str());
    TEST_ASSERT_EQUAL_UINT32(123, captured.selected_bytes);
    TEST_ASSERT_EQUAL_UINT32(456, cache.capture().selected_bytes);
}

void test_status_publication_preserves_selection_and_prior_snapshots()
{
    std::mutex mutex;
    AppStatus::SnapshotCache<std::mutex> cache(mutex);
    cache.selectFile("payload.bin", 100);
    AppStatus::RadioSnapshot progress{};
    progress.radio.state = RadioState::TxBusy;
    progress.tx.bytes_transferred = 20;
    progress.tx.total_bytes = 100;
    cache.publish(progress);
    const auto first = cache.capture();
    progress.tx.bytes_transferred = 80;
    cache.publish(progress);
    const auto next = cache.capture();

    TEST_ASSERT_EQUAL_UINT32(20, first.status.tx.bytes_transferred);
    TEST_ASSERT_EQUAL_UINT32(80, next.status.tx.bytes_transferred);
    TEST_ASSERT_EQUAL_STRING("payload.bin", next.selected_name.c_str());
    TEST_ASSERT_EQUAL_UINT32(100, next.selected_bytes);
    TEST_ASSERT_TRUE(next.status.radio.state == RadioState::TxBusy);
}

void test_status_capture_remains_consistent_during_progress_and_selection_updates()
{
    std::mutex mutex;
    AppStatus::SnapshotCache<std::mutex> cache(mutex);
    cache.selectFile("file-0", 0);
    std::atomic_bool finished{false};
    std::atomic_bool consistent{true};
    std::thread producer([&]() {
        for (uint32_t i = 1; i <= 2000; ++i) {
            AppStatus::RadioSnapshot progress{};
            progress.tx.current_sequence = i;
            progress.tx.bytes_transferred = i * 20;
            progress.updated_ms = i;
            cache.publish(progress);
            cache.selectFile("file-" + std::to_string(i), i);
        }
        finished.store(true);
    });
    do {
        const auto captured = cache.capture();
        if (captured.selected_name != "file-" + std::to_string(captured.selected_bytes) ||
            captured.status.tx.bytes_transferred != captured.status.tx.current_sequence * 20 ||
            captured.status.updated_ms != captured.status.tx.current_sequence) {
            consistent.store(false);
        }
    } while (!finished.load());
    producer.join();

    TEST_ASSERT_TRUE(consistent.load());
    TEST_ASSERT_EQUAL_UINT32(40000, cache.capture().status.tx.bytes_transferred);
}

void test_json_string_escapes_quotes_backslashes_and_all_control_bytes()
{
    std::string input = "\"\\";
    std::string expected = "\"\\\"\\\\";
    constexpr char hex[] = "0123456789abcdef";
    for (unsigned ch = 0; ch < 32; ++ch) {
        input += static_cast<char>(ch);
        expected += "\\u00";
        expected += hex[ch >> 4];
        expected += hex[ch & 15];
    }
    input += "caf\xc3\xa9.bin";
    expected += "caf\xc3\xa9.bin\"";
    std::string encoded;
    AppStatus::appendJsonString(encoded, input);
    TEST_ASSERT_EQUAL_STRING(expected.c_str(), encoded.c_str());
}

void test_status_json_retains_transfer_progress_and_encodes_owned_text()
{
    std::mutex mutex;
    AppStatus::SnapshotCache<std::mutex> cache(mutex);
    cache.selectFile("quote\"line\n.bin", 100);
    AppStatus::RadioSnapshot progress{};
    progress.radio.state = RadioState::RxListening;
    progress.radio.last_fault = 10;
    progress.tx.bytes_transferred = 40;
    progress.tx.total_bytes = 100;
    progress.tx.retry_count = 2;
    progress.tx.throughput_bps = 50;
    progress.updated_ms = 12345;
    cache.publish(progress);
    const auto snapshot = cache.capture();
    cache.selectFile("changed.bin", 1);
    const std::string json = AppStatus::buildJson(snapshot, "node\"one", "host\\one");

    TEST_ASSERT_NOT_NULL(std::strstr(json.c_str(), "\"selected\":\"quote\\\"line\\u000a.bin\""));
    TEST_ASSERT_NOT_NULL(std::strstr(json.c_str(), "\"node_name\":\"node\\\"one\""));
    TEST_ASSERT_NOT_NULL(std::strstr(json.c_str(), "\"hostname\":\"host\\\\one\""));
    TEST_ASSERT_NOT_NULL(std::strstr(json.c_str(), "\"selected_bytes\":100"));
    TEST_ASSERT_NOT_NULL(std::strstr(json.c_str(), "\"tx_bytes\":40"));
    TEST_ASSERT_NOT_NULL(std::strstr(json.c_str(), "\"tx_total_bytes\":100"));
    TEST_ASSERT_NOT_NULL(std::strstr(json.c_str(), "\"tx_retries\":2"));
    TEST_ASSERT_NOT_NULL(std::strstr(json.c_str(), "\"tx_bytes_per_second\":50"));
    TEST_ASSERT_NOT_NULL(std::strstr(json.c_str(), "\"snapshot_updated_ms\":12345"));
    TEST_ASSERT_NOT_NULL(std::strstr(json.c_str(), "\"last_fault\":10"));
    TEST_ASSERT_NOT_NULL(std::strstr(json.c_str(), "\"peer_complete\":false"));
    TEST_ASSERT_NULL(std::strstr(json.c_str(), "changed.bin"));
}

} // namespace

void runStatusSnapshotTests()
{
    RUN_TEST(test_status_snapshot_owns_filename_after_selection_changes);
    RUN_TEST(test_status_publication_preserves_selection_and_prior_snapshots);
    RUN_TEST(test_status_capture_remains_consistent_during_progress_and_selection_updates);
    RUN_TEST(test_json_string_escapes_quotes_backslashes_and_all_control_bytes);
    RUN_TEST(test_status_json_retains_transfer_progress_and_encodes_owned_text);
}
