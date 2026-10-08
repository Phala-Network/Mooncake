#include "master_service.h"
#include "metadata_key_budget.h"
#include "storage_backend.h"

#include <gtest/gtest.h>

#include <atomic>
#include <filesystem>
#include <thread>
#include <unistd.h>

#include "utils.h"

namespace mooncake::test {

TEST(MetadataKeyBudgetReservation, ConcurrentReservationsNeverOverbook) {
    MetadataKeyBudget budget(7);
    std::atomic<bool> go{false};
    std::atomic<int> winners{0};
    std::atomic<int> attempted{0};
    std::vector<std::thread> threads;
    for (int i = 0; i < 32; ++i) {
        threads.emplace_back([&] {
            while (!go.load()) std::this_thread::yield();
            auto slot = budget.TryAcquire();
            if (slot) ++winners;
            ++attempted;
            while (attempted.load() != 32) std::this_thread::yield();
        });
    }
    go = true;
    for (auto& thread : threads) thread.join();
    EXPECT_EQ(winners.load(), 7);
    EXPECT_EQ(budget.Used(), 0);
    auto all = budget.TryAcquire(7);
    ASSERT_TRUE(all);
    EXPECT_FALSE(budget.TryAcquire());
    auto one = all->TakeOne();
    all.reset();
    EXPECT_EQ(budget.Used(), 1);
    one = {};
    EXPECT_EQ(budget.Used(), 0);
}

class MetadataKeyBudgetTest : public ::testing::Test {
   protected:
    uint64_t Used(MasterService& service) {
        return service.metadata_key_budget_.Used();
    }
    std::vector<uint8_t> Snapshot(MasterService& service) {
        MasterService::MetadataSerializer serializer(&service);
        auto result = serializer.Serialize();
        EXPECT_TRUE(result);
        return result ? std::move(*result) : std::vector<uint8_t>{};
    }
    bool Restore(MasterService& service, const std::vector<uint8_t>& data) {
        MasterService::MetadataSerializer serializer(&service);
        return serializer.Deserialize(data).has_value();
    }
    UUID Mount(MasterService& service) {
        Segment segment;
        segment.id = generate_uuid();
        segment.name = "metadata-budget-segment";
        segment.base = 0x300000000;
        segment.size = 16 * 1024 * 1024;
        segment.te_endpoint = segment.name;
        auto client = generate_uuid();
        EXPECT_TRUE(service.MountSegment(segment, client));
        return client;
    }
};

TEST_F(MetadataKeyBudgetTest, PendingWritesFailureRevokeAndExistingRead) {
    MasterServiceConfig config;
    config.metadata_key_limit = 2;
    config.default_kv_lease_ttl = 0;
    MasterService service(config);
    ReplicateConfig replication;
    auto client = generate_uuid();
    // Failed allocation must release its admission reservation.
    EXPECT_FALSE(service.PutStart(client, "no-segment", TenantId::Default(),
                                  1024, replication));
    EXPECT_EQ(Used(service), 0);
    client = Mount(service);
    ASSERT_TRUE(
        service.PutStart(client, "a", TenantId::Default(), 1024, replication));
    ASSERT_TRUE(
        service.PutStart(client, "b", TenantId::Default(), 1024, replication));
    EXPECT_EQ(Used(service), 2);
    auto full =
        service.PutStart(client, "c", TenantId::Default(), 1024, replication);
    ASSERT_FALSE(full);
    EXPECT_EQ(full.error(), ErrorCode::NO_AVAILABLE_HANDLE);
    auto duplicate =
        service.PutStart(client, "a", TenantId::Default(), 1024, replication);
    ASSERT_FALSE(duplicate);
    EXPECT_EQ(duplicate.error(), ErrorCode::OBJECT_ALREADY_EXISTS);
    ASSERT_TRUE(
        service.PutEnd(client, "a", TenantId::Default(), ReplicaType::MEMORY));
    EXPECT_TRUE(service.GetReplicaList("a", TenantId::Default()));
    ASSERT_TRUE(service.PutRevoke(client, "b", TenantId::Default(),
                                  ReplicaType::MEMORY));
    EXPECT_EQ(Used(service), 1);
    EXPECT_TRUE(
        service.PutStart(client, "c", TenantId::Default(), 1024, replication));
    EXPECT_EQ(Used(service), 2);
}

TEST_F(MetadataKeyBudgetTest, ConcurrentPutsAcrossShardsRespectTotalLimit) {
    MasterServiceConfig config;
    config.metadata_key_limit = 4;
    MasterService service(config);
    auto client = Mount(service);
    std::atomic<bool> go{false};
    std::atomic<int> successes{0};
    std::vector<std::thread> threads;
    for (int i = 0; i < 32; ++i) {
        threads.emplace_back([&, i] {
            while (!go.load()) std::this_thread::yield();
            auto result =
                service.PutStart(client, "parallel-" + std::to_string(i),
                                 TenantId::Default(), 1024, ReplicateConfig{});
            if (result) {
                ++successes;
            } else {
                EXPECT_EQ(result.error(), ErrorCode::NO_AVAILABLE_HANDLE);
            }
        });
    }
    go = true;
    for (auto& thread : threads) thread.join();
    EXPECT_EQ(successes.load(), 4);
    EXPECT_EQ(Used(service), 4);
}

TEST_F(MetadataKeyBudgetTest, DiskRegistrationAndSnapshotAlsoConsumeSlots) {
    MasterServiceConfig config;
    config.metadata_key_limit = 1;
    MasterService service(config);
    auto client = generate_uuid();
    Replica disk(client, 1024, "disk-endpoint", ReplicaStatus::COMPLETE);
    ASSERT_TRUE(
        service.AddReplica(client, "disk-a", TenantId::Default(), disk));
    EXPECT_EQ(Used(service), 1);
    // Refreshing the existing disk replica must not need another slot.
    EXPECT_TRUE(
        service.AddReplica(client, "disk-a", TenantId::Default(), disk));
    auto full = service.AddReplica(client, "disk-b", TenantId::Default(), disk);
    ASSERT_FALSE(full);
    EXPECT_EQ(full.error(), ErrorCode::NO_AVAILABLE_HANDLE);
    EXPECT_EQ(Used(service), 1);
    auto snapshot = Snapshot(service);
    MasterService restored(config);
    ASSERT_TRUE(Restore(restored, snapshot));
    EXPECT_EQ(Used(restored), 1);
    ASSERT_TRUE(Restore(restored, snapshot));
    EXPECT_EQ(Used(restored), 1);
    EXPECT_FALSE(
        restored.AddReplica(client, "disk-b", TenantId::Default(), disk));
}

class BucketKeyBudgetTest : public ::testing::Test {
   protected:
    void SetUp() override {
        static std::atomic<int> sequence{0};
        path = std::filesystem::temp_directory_path() /
               ("mooncake-key-budget-" + std::to_string(getpid()) + "-" +
                std::to_string(sequence++));
        std::filesystem::create_directories(path);
    }
    void TearDown() override { std::filesystem::remove_all(path); }
    std::filesystem::path path;
    std::string value = std::string(1024, 'x');
    auto Batch(std::initializer_list<const char*> keys) {
        std::unordered_map<std::string, std::vector<Slice>> batch;
        for (auto key : keys)
            batch.emplace(key,
                          std::vector<Slice>{{value.data(), value.size()}});
        return batch;
    }
    static ErrorCode Complete(const std::vector<std::string>&,
                              std::vector<StorageObjectMetadata>&) {
        return ErrorCode::OK;
    }
};

TEST_F(BucketKeyBudgetTest, LruEvictsForKeysBelowByteCapAndCanContinue) {
    FileStorageConfig config;
    config.storage_filepath = path.string();
    config.total_keys_limit = 2;
    BucketBackendConfig buckets;
    buckets.eviction_policy = BucketEvictionPolicy::LRU;
    buckets.max_total_size = 1024 * 1024;
    BucketStorageBackend backend(config, buckets);
    ASSERT_TRUE(backend.Init());
    ASSERT_TRUE(backend.BatchOffload(Batch({"a", "b"}), Complete));
    std::vector<std::string> evicted;
    auto notify = [&](const std::vector<std::string>& keys)
        -> tl::expected<void, ErrorCode> {
        evicted.insert(evicted.end(), keys.begin(), keys.end());
        // The previous bucket's file still exists until notification succeeds.
        bool has_data = false;
        for (const auto& file : std::filesystem::directory_iterator(path))
            has_data |= file.is_regular_file();
        EXPECT_TRUE(has_data);
        return {};
    };
    ASSERT_TRUE(backend.BatchOffload(Batch({"c"}), Complete, notify));
    EXPECT_EQ(evicted.size(), 2);
    EXPECT_FALSE(*backend.IsExist("a"));
    EXPECT_TRUE(*backend.IsExist("c"));
    EXPECT_TRUE(*backend.IsEnableOffloading());
    ASSERT_TRUE(backend.BatchOffload(Batch({"d"}), Complete, notify));
    EXPECT_TRUE(*backend.IsExist("c"));
    EXPECT_TRUE(*backend.IsExist("d"));
}

TEST_F(BucketKeyBudgetTest, FailedNotificationPreservesConcurrentReservation) {
    FileStorageConfig config;
    config.storage_filepath = path.string();
    config.total_keys_limit = 2;
    BucketBackendConfig buckets;
    buckets.eviction_policy = BucketEvictionPolicy::LRU;
    buckets.max_total_size = 1024 * 1024;
    BucketStorageBackend backend(config, buckets);
    ASSERT_TRUE(backend.Init());
    ASSERT_TRUE(backend.BatchOffload(Batch({"old"}), Complete));
    auto failed = backend.BatchOffload(
        Batch({"new-a", "new-b"}), Complete,
        [&](const std::vector<std::string>&) -> tl::expected<void, ErrorCode> {
            auto concurrent =
                backend.BatchOffload(Batch({"concurrent"}), Complete);
            EXPECT_FALSE(concurrent);
            if (!concurrent) {
                EXPECT_EQ(concurrent.error(), ErrorCode::FILE_WRITE_FAIL);
            }
            return tl::make_unexpected(ErrorCode::INTERNAL_ERROR);
        });
    ASSERT_FALSE(failed);
    EXPECT_TRUE(*backend.IsExist("old"));
    EXPECT_FALSE(*backend.IsExist("new-a"));
    EXPECT_FALSE(*backend.IsExist("concurrent"));
    ASSERT_TRUE(backend.BatchOffload(Batch({"after"}), Complete));
    EXPECT_TRUE(*backend.IsExist("old"));
    EXPECT_TRUE(*backend.IsExist("after"));
}

TEST_F(BucketKeyBudgetTest, PendingWriteAndCompletionFailureReleaseSlots) {
    FileStorageConfig config;
    config.storage_filepath = path.string();
    config.total_keys_limit = 1;
    BucketBackendConfig buckets;
    buckets.eviction_policy = BucketEvictionPolicy::LRU;
    buckets.max_total_size = 1024 * 1024;
    BucketStorageBackend backend(config, buckets);
    ASSERT_TRUE(backend.Init());
    // An incoming bucket larger than the key budget must leave no reservation.
    EXPECT_FALSE(
        backend.BatchOffload(Batch({"too-big-a", "too-big-b"}), Complete));
    auto failed = backend.BatchOffload(Batch({"will-fail"}),
                                       [](const std::vector<std::string>&,
                                          std::vector<StorageObjectMetadata>&) {
                                           return ErrorCode::INTERNAL_ERROR;
                                       });
    EXPECT_FALSE(failed);
    EXPECT_FALSE(*backend.IsExist("will-fail"));
    EXPECT_TRUE(backend.BatchOffload(Batch({"retry"}), Complete));
}

}  // namespace mooncake::test
