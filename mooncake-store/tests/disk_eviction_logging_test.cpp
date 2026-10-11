#include <glog/logging.h>
#include <gtest/gtest.h>

#include <mutex>
#include <string>
#include <vector>

#include "rpc_service.h"
#include "utils/disk_eviction_error_summary.h"

namespace mooncake {
namespace {

class EvictionLogSink : public google::LogSink {
   public:
    void send(google::LogSeverity severity, const char*, const char*, int,
              const struct tm*, const char* message, size_t length) override {
        if (severity < google::GLOG_INFO) return;
        std::lock_guard<std::mutex> lock(mutex_);
        messages_.emplace_back(message, length);
    }

    std::vector<std::string> Matching(const std::string& needle) {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<std::string> result;
        for (const auto& message : messages_) {
            if (message.find(needle) != std::string::npos) {
                result.push_back(message);
            }
        }
        return result;
    }

   private:
    std::mutex mutex_;
    std::vector<std::string> messages_;
};

class DiskEvictionLoggingTest : public ::testing::Test {
   protected:
    void SetUp() override {
        google::InitGoogleLogging("DiskEvictionLoggingTest");
        saved_v_ = FLAGS_v;
        FLAGS_v = 0;
        google::AddLogSink(&sink_);
    }
    void TearDown() override {
        google::RemoveLogSink(&sink_);
        FLAGS_v = saved_v_;
        google::ShutdownGoogleLogging();
    }
    EvictionLogSink sink_;
    int saved_v_ = 0;
};

TEST_F(DiskEvictionLoggingTest, LargeTransportFailureHasOneBoundedDiagnostic) {
    DiskEvictionErrorSummary errors;
    for (size_t i = 0; i < 150000; ++i) {
        errors.Record(ErrorCode::RPC_TIMEOUT, i);
    }
    errors.Log("transport_fixture", 150000);
    const auto logs = sink_.Matching("transport_fixture");
    ASSERT_EQ(logs.size(), 1);
    EXPECT_NE(logs[0].find("RPC_TIMEOUT"), std::string::npos);
    EXPECT_NE(logs[0].find("count=150000"), std::string::npos);
    EXPECT_NE(logs[0].find("first_key_index=0"), std::string::npos);
    EXPECT_LT(logs[0].size(), 200);
}

TEST_F(DiskEvictionLoggingTest, MixedErrorsHaveExactCountsAndFirstSamples) {
    DiskEvictionErrorSummary errors;
    errors.Record(ErrorCode::OBJECT_NOT_FOUND, 0);
    errors.Record(ErrorCode::RPC_TIMEOUT, 2);
    errors.Record(ErrorCode::RPC_FAIL, 3);
    errors.Record(ErrorCode::RPC_TIMEOUT, 4);
    errors.Record(ErrorCode::OBJECT_NOT_FOUND, 5);
    errors.Log("mixed_fixture", 6);
    EXPECT_EQ(sink_.Matching("mixed_fixture").size(), 2);
    const auto timeouts = sink_.Matching("RPC_TIMEOUT");
    ASSERT_EQ(timeouts.size(), 1);
    EXPECT_NE(timeouts[0].find("count=2"), std::string::npos);
    EXPECT_NE(timeouts[0].find("first_key_index=2"), std::string::npos);
    const auto failures = sink_.Matching("RPC_FAIL");
    ASSERT_EQ(failures.size(), 1);
    EXPECT_NE(failures[0].find("count=1"), std::string::npos);
    EXPECT_NE(failures[0].find("first_key_index=3"), std::string::npos);
    EXPECT_TRUE(sink_.Matching("OBJECT_NOT_FOUND").empty());
}

TEST_F(DiskEvictionLoggingTest, EmptySummaryIsSilent) {
    DiskEvictionErrorSummary{}.Log("empty_fixture", 0);
    EXPECT_TRUE(sink_.Matching("empty_fixture").empty());
}

TEST_F(DiskEvictionLoggingTest,
       RealBatchMissingKeysPreserveResultsAndStayQuiet) {
    WrappedMasterServiceConfig config;
    config.default_kv_lease_ttl = 0;
    config.enable_metric_reporting = false;
    WrappedMasterService service(config);
    const std::vector<std::string> keys(4096, "missing_eviction_fixture_key");
    for (int attempt = 0; attempt < 2; ++attempt) {
        const auto results = service.BatchEvictDiskReplica(
            UUID{}, keys, TenantId::Default().value(), ReplicaType::LOCAL_DISK);
        ASSERT_EQ(results.size(), keys.size());
        for (const auto& result : results) {
            ASSERT_FALSE(result.has_value());
            EXPECT_EQ(result.error(), ErrorCode::OBJECT_NOT_FOUND);
        }
    }
    EXPECT_TRUE(sink_.Matching("object_not_found_for_eviction").empty());
    EXPECT_TRUE(sink_.Matching("BatchEvictDiskReplica failed for key").empty());
    EXPECT_TRUE(sink_.Matching("missing_eviction_fixture_key").empty());
    EXPECT_TRUE(sink_.Matching("BatchEvictDiskReplica: error=").empty());
}

TEST_F(DiskEvictionLoggingTest,
       RealInvalidTypeBatchIsAggregatedAndDoesNotEvict) {
    WrappedMasterServiceConfig config;
    config.default_kv_lease_ttl = 0;
    config.enable_metric_reporting = false;
    config.enable_offload = true;
    WrappedMasterService service(config);
    const UUID client_id{42, 17};
    ASSERT_TRUE(service.MountLocalDiskSegment(client_id, true));
    const std::string key = "registered_eviction_fixture_key";
    OffloadTaskItem task{
        .tenant_id = TenantId::Default().value(), .key = key, .size = 128};
    StorageObjectMetadata metadata;
    metadata.data_size = 128;
    metadata.transport_endpoint = "127.0.0.1:12345";
    ASSERT_TRUE(service.NotifyOffloadSuccess(client_id, {task}, {metadata}));

    const std::vector<std::string> keys(4096, key);
    const auto results = service.BatchEvictDiskReplica(
        client_id, keys, TenantId::Default().value(), ReplicaType::MEMORY);
    ASSERT_EQ(results.size(), keys.size());
    for (const auto& result : results) {
        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error(), ErrorCode::INVALID_PARAMS);
    }
    const auto logs = sink_.Matching("BatchEvictDiskReplica: error=");
    ASSERT_EQ(logs.size(), 1);
    EXPECT_NE(logs[0].find("count=4096"), std::string::npos);
    EXPECT_NE(logs[0].find("first_key_index=0"), std::string::npos);
    EXPECT_TRUE(sink_.Matching("invalid_replica_type_for_eviction").empty());

    // The error path must not mutate the replica. A following valid eviction
    // succeeds once, then reports the same idempotent missing result as before.
    auto valid = service.BatchEvictDiskReplica(
        client_id, {key}, TenantId::Default().value(), ReplicaType::LOCAL_DISK);
    ASSERT_EQ(valid.size(), 1);
    ASSERT_TRUE(valid[0]);
    auto repeated = service.BatchEvictDiskReplica(
        client_id, {key}, TenantId::Default().value(), ReplicaType::LOCAL_DISK);
    ASSERT_EQ(repeated.size(), 1);
    ASSERT_FALSE(repeated[0]);
    EXPECT_EQ(repeated[0].error(), ErrorCode::OBJECT_NOT_FOUND);
}

}  // namespace
}  // namespace mooncake
