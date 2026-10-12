// Deterministic promotion orchestration tests: block the SSD read with a latch,
// observe the independent heartbeat, and release I/O before destroying buffers.
#include <glog/logging.h>
#include <gtest/gtest.h>
#include <atomic>
#include <condition_variable>
#include <future>
#include <filesystem>
#include <mutex>
#include "client_service.h"
#include "file_storage.h"
#include "storage_backend.h"

namespace mooncake {
namespace fs_test {
class FakeBackend : public StorageBackendInterface {
   public:
    explicit FakeBackend(const FileStorageConfig& config)
        : StorageBackendInterface(config) {}
    std::mutex mutex;
    std::condition_variable cv;
    bool blocked{false};
    bool entered{false};
    ErrorCode load_error{ErrorCode::OK};
    tl::expected<void, ErrorCode> Init() override { return {}; }
    tl::expected<int64_t, ErrorCode> BatchOffload(
        const std::unordered_map<std::string, std::vector<Slice>>&,
        std::function<ErrorCode(const std::vector<std::string>&,
                                std::vector<StorageObjectMetadata>&)>,
        EvictionHandler) override {
        return int64_t{0};
    }
    tl::expected<void, ErrorCode> BatchLoad(
        std::unordered_map<std::string, Slice>&) override {
        std::unique_lock lock(mutex);
        entered = true;
        cv.notify_all();
        cv.wait(lock, [&] { return !blocked; });
        if (load_error != ErrorCode::OK) return tl::make_unexpected(load_error);
        return {};
    }
    tl::expected<bool, ErrorCode> IsExist(const std::string&) override {
        return true;
    }
    tl::expected<bool, ErrorCode> IsEnableOffloading() override {
        return false;
    }
    tl::expected<void, ErrorCode> ScanMeta(
        const std::function<ErrorCode(const std::vector<std::string>&,
                                      std::vector<StorageObjectMetadata>&)>&)
        override {
        return {};
    }
    void Release() {
        std::lock_guard lock(mutex);
        blocked = false;
        cv.notify_all();
    }
    bool WaitEntered() {
        std::unique_lock lock(mutex);
        return cv.wait_for(lock, std::chrono::seconds(3),
                           [&] { return entered; });
    }
};

class FakeClient : public Client {
   public:
    FakeClient() : Client("localhost:9003", "", "tcp", {}) {}
    std::vector<PromotionTaskItemV2> queue;
    size_t batch_size{1};
    ErrorCode heartbeat_error{ErrorCode::OK};
    ErrorCode alloc_error{ErrorCode::OK};
    ErrorCode write_error{ErrorCode::OK};
    ErrorCode success_error{ErrorCode::OK};
    std::atomic<int> polls{0}, offload_polls{0}, allocations{0}, writes{0},
        successes{0}, failures{0};
    UUID last_token{};
    std::mutex mutex;
    std::condition_variable cv;
    tl::expected<void, ErrorCode> PromotionObjectHeartbeatV2(
        std::vector<PromotionTaskItemV2>& tasks) override {
        std::lock_guard lock(mutex);
        ++polls;
        cv.notify_all();
        if (heartbeat_error != ErrorCode::OK)
            return tl::make_unexpected(heartbeat_error);
        while (!queue.empty() && tasks.size() < batch_size) {
            tasks.push_back(queue.back());
            queue.pop_back();
        }
        return {};
    }
    tl::expected<void, ErrorCode> OffloadObjectHeartbeat(
        bool, std::vector<OffloadTaskItem>&) override {
        ++offload_polls;
        return {};
    }
    tl::expected<bool, ErrorCode> PollRemoveAll() override { return false; }
    tl::expected<PromotionAllocStartResponse, ErrorCode> PromotionAllocStartV2(
        const std::string&, const std::string&, uint64_t,
        const std::vector<std::string>&, const UUID& token) override {
        ++allocations;
        last_token = token;
        if (alloc_error != ErrorCode::OK)
            return tl::make_unexpected(alloc_error);
        return PromotionAllocStartResponse{};
    }
    ErrorCode PromotionWrite(const Replica::Descriptor&,
                             std::vector<Slice>&) override {
        ++writes;
        return write_error;
    }
    tl::expected<void, ErrorCode> NotifyPromotionSuccessV2(
        const std::string&, const std::string&, const UUID& token) override {
        last_token = token;
        ++successes;
        if (success_error != ErrorCode::OK)
            return tl::make_unexpected(success_error);
        return {};
    }
    tl::expected<void, ErrorCode> NotifyPromotionFailureV2(
        const std::string&, const std::string&, const UUID& token) override {
        last_token = token;
        ++failures;
        return {};
    }
    bool WaitPolls(int count) {
        std::unique_lock lock(mutex);
        return cv.wait_for(lock, std::chrono::seconds(3),
                           [&] { return polls >= count; });
    }
};
}  // namespace fs_test

class FileStoragePromotionTest : public ::testing::Test {
   protected:
    std::shared_ptr<fs_test::FakeClient> client;
    std::shared_ptr<fs_test::FakeBackend> backend;
    std::unique_ptr<FileStorage> storage;
    const UUID token{17, 29};
    void SetUp() override {
        google::InitGoogleLogging("FileStoragePromotionTest");
        FileStorageConfig cfg;
        cfg.storage_filepath = std::filesystem::temp_directory_path().string();
        cfg.local_buffer_size = 4 * 1024 * 1024;
        cfg.heartbeat_interval_seconds =
            30;  // tests must not wait for this interval
        client = std::make_shared<fs_test::FakeClient>();
        backend = std::make_shared<fs_test::FakeBackend>(cfg);
        storage = std::make_unique<FileStorage>(cfg, client, "localhost:9003");
        storage->storage_backend_ = backend;
        storage->enable_offloading_ = false;
    }
    void TearDown() override {
        if (backend) backend->Release();
        storage.reset();
        client.reset();
        backend.reset();
        google::ShutdownGoogleLogging();
    }
    void Queue(int count = 1, int64_t size = 1024) {
        for (int i = 0; i < count; ++i)
            client->queue.push_back(
                {"default", "key" + std::to_string(i), size, token});
    }
    auto Process(bool* progressed = nullptr) {
        return storage->ProcessPromotionTasks(progressed);
    }
    void Start() { storage->StartPromotionWorker(); }
    void Stop() { storage->StopPromotionWorker(); }
    auto Heartbeat() { return storage->Heartbeat(); }
    void Drain() { storage->draining_.store(true); }
};

TEST_F(FileStoragePromotionTest, EmptyAndRpcErrorsBackOffWithoutAllocation) {
    bool progressed = true;
    EXPECT_TRUE(Process(&progressed));
    EXPECT_FALSE(progressed);
    client->heartbeat_error = ErrorCode::SEGMENT_NOT_FOUND;
    EXPECT_TRUE(Process());
    client->heartbeat_error = ErrorCode::INTERNAL_ERROR;
    EXPECT_FALSE(Process());
    EXPECT_EQ(client->allocations, 0);
}

TEST_F(FileStoragePromotionTest, InvalidSizeReleasesAdmissionWithSameToken) {
    Queue(1, 0);
    EXPECT_TRUE(Process());
    EXPECT_EQ(client->allocations, 0);
    EXPECT_EQ(client->failures, 1);
    EXPECT_EQ(client->last_token, token);
}

TEST_F(FileStoragePromotionTest, ReadFailureDoesNotAllocateRemoteMemory) {
    Queue();
    backend->load_error = ErrorCode::OBJECT_NOT_FOUND;
    EXPECT_TRUE(Process());
    EXPECT_EQ(client->allocations, 0);
    EXPECT_EQ(client->writes, 0);
    EXPECT_EQ(client->failures, 1);
    EXPECT_EQ(client->last_token, token);
}

TEST_F(FileStoragePromotionTest,
       AllocationWriteAndCommitFailuresReleaseExactlyOnce) {
    for (int failure = 0; failure < 3; ++failure) {
        Queue();
        client->alloc_error =
            failure == 0 ? ErrorCode::NO_AVAILABLE_HANDLE : ErrorCode::OK;
        client->write_error =
            failure == 1 ? ErrorCode::TRANSFER_FAIL : ErrorCode::OK;
        client->success_error =
            failure == 2 ? ErrorCode::INTERNAL_ERROR : ErrorCode::OK;
        bool progressed = true;
        EXPECT_TRUE(Process(&progressed));
        EXPECT_FALSE(progressed);
        EXPECT_EQ(client->failures, failure + 1);
    }
    EXPECT_EQ(client->allocations, 3);
    EXPECT_EQ(client->writes, 2);
    EXPECT_EQ(client->successes, 1);
    EXPECT_EQ(client->last_token, token);
}

TEST_F(FileStoragePromotionTest,
       SuccessfulBacklogDrainsWithoutHeartbeatInterval) {
    Queue(5);
    Start();
    ASSERT_TRUE(
        client->WaitPolls(6));  // 5 successful tasks then empty; no 30s wait
    Stop();
    EXPECT_EQ(client->successes, 5);
    EXPECT_EQ(client->failures, 0);
    EXPECT_EQ(client->polls, 6);  // idle worker does not spin
}

TEST_F(FileStoragePromotionTest,
       SlowSsdReadDoesNotBlockOffloadHeartbeatOrAllocateTarget) {
    Queue();
    backend->blocked = true;
    Start();
    ASSERT_TRUE(backend->WaitEntered());
    EXPECT_EQ(client->allocations, 0);
    auto heartbeat =
        std::async(std::launch::async, [&] { return Heartbeat(); });
    const auto status = heartbeat.wait_for(std::chrono::seconds(2));
    backend->Release();  // unblock even on failure so the test cannot hang
    EXPECT_EQ(status, std::future_status::ready);
    EXPECT_TRUE(heartbeat.get());
    Stop();
    EXPECT_EQ(client->offload_polls, 1);
}

TEST_F(FileStoragePromotionTest, ShutdownWaitsForIoAndPreventsNewTask) {
    Queue(3);
    client->batch_size = 3;
    backend->blocked = true;
    Start();
    ASSERT_TRUE(backend->WaitEntered());
    auto stop = std::async(std::launch::async, [&] { Stop(); });
    EXPECT_EQ(stop.wait_for(std::chrono::milliseconds(50)),
              std::future_status::timeout);
    backend->Release();
    EXPECT_EQ(stop.wait_for(std::chrono::seconds(2)),
              std::future_status::ready);
    stop.get();
    EXPECT_EQ(client->polls, 1);
    EXPECT_EQ(client->allocations, 0);
    EXPECT_EQ(client->failures, 3);
}

TEST_F(FileStoragePromotionTest,
       DrainDuringReadReleasesWithoutAllocatingTarget) {
    Queue();
    backend->blocked = true;
    Start();
    ASSERT_TRUE(backend->WaitEntered());
    Drain();
    backend->Release();
    Stop();
    EXPECT_EQ(client->allocations, 0);
    EXPECT_EQ(client->failures, 1);
}

TEST_F(FileStoragePromotionTest, AllocationPressureDoesNotHotLoop) {
    Queue(5);
    client->alloc_error = ErrorCode::NO_AVAILABLE_HANDLE;
    Start();
    ASSERT_TRUE(client->WaitPolls(1));
    Stop();
    EXPECT_EQ(client->polls, 1);
    EXPECT_EQ(client->failures, 1);
}
}  // namespace mooncake

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
