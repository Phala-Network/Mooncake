#include <glog/logging.h>
#include <gtest/gtest.h>
#include <json/json.h>
#include <sys/stat.h>
#include <unistd.h>
#include <filesystem>
#include <fstream>
#include <thread>
#include <ylt/coro_http/coro_http_client.hpp>
#include "master_admin_service.h"
#include "master_client.h"
#include "rpc_service.h"
#include "tenant_quota_policy_store.h"
#include "utils.h"

namespace mooncake::test {
class FiniteSnapshotTest : public ::testing::Test {
   protected:
    const std::string tenant = "ds101-dsv4-nvfp4-static-50c8";
    const std::string key = "private-native-key";
    std::filesystem::path root;
    std::shared_ptr<WrappedMasterService> service;
    UUID writer = generate_uuid();
    Segment segment;
    shared_cache_diagnostics::Config identity;
    void SetUp() override {
        root = std::filesystem::temp_directory_path() /
               UuidToString(generate_uuid());
        std::filesystem::create_directories(root);
        TenantQuotaPolicySnapshot policy;
        policy.tenant_quotas = {{tenant, 1024 * 1024},
                                {"tenant-b", 1024 * 1024},
                                {"default", 1024 * 1024}};
        std::ofstream(root / "quota.yaml")
            << FormatTenantQuotaPolicyYaml(policy);
        WrappedMasterServiceConfig config;
        config.enable_metric_reporting = false;
        config.enable_multi_tenants = true;
        config.enable_offload = true;
        config.promotion_on_hit = true;
        config.promotion_admission_threshold = 1;
        config.default_kv_lease_ttl = 300;
        config.tenant_quota_connector_type = "file";
        config.tenant_quota_connector_uri = (root / "quota.yaml").string();
        service = std::make_shared<WrappedMasterService>(config);
        segment.id = generate_uuid();
        segment.name = "private-memory-segment";
        segment.base = 0x600000000;
        segment.size = 16 * 1024 * 1024;
        ASSERT_TRUE(service->MountSegment(segment, writer));
        ASSERT_TRUE(service->MountLocalDiskSegment(writer, true));
        identity.case_id = "case1";
        identity.epoch = "epoch1";
        identity.tenant_id = tenant;
        identity.key_salt = "fixture-salt-0123456789";
    }
    void TearDown() override {
        service.reset();
        std::filesystem::remove_all(root);
        unsetenv("MOONCAKE_SHARED_CACHE_SNAPSHOT_MANIFEST");
    }
    std::string Id(const std::string& domain, const std::string& value) {
        return shared_cache_diagnostics::Identifier(identity, domain, tenant,
                                                    value);
    }
    shared_cache_diagnostics::SnapshotRequest Request(const std::string& t) {
        shared_cache_diagnostics::SnapshotRequest q;
        q.identity = identity;
        q.identity.tenant_id = t;
        q.sample_id = "direct";
        q.keys = {{shared_cache_diagnostics::Identifier(
                       q.identity, "phala.shared-cache-key.v1", t, key),
                   key}};
        q.max_response_bytes = 65536;
        q.max_total_logical_bytes = 1048576;
        q.deadline =
            std::chrono::steady_clock::now() + std::chrono::seconds(10);
        return q;
    }
    Json::Value Snapshot(const std::string& t) {
        auto output = service->GetFiniteSnapshotForAdmin(Request(t));
        EXPECT_TRUE(output);
        Json::Value value;
        if (output) {
            Json::Reader reader;
            EXPECT_TRUE(reader.parse(*output, value));
        }
        return value;
    }
    void Put(const std::string& t, bool complete = true) {
        ReplicateConfig config;
        config.replica_num = 1;
        ASSERT_TRUE(service->PutStart(writer, key, 1024, config, t));
        if (complete)
            ASSERT_TRUE(service->PutEnd(writer, ObjectMeta{key, std::nullopt},
                                        ReplicaType::MEMORY, t));
    }
    void Disk(const std::string& t) {
        StorageObjectMetadata metadata;
        metadata.bucket_id = 1;
        metadata.offset = 0;
        metadata.key_size = key.size();
        metadata.data_size = 1024;
        metadata.transport_endpoint = "private-owner-endpoint";
        ASSERT_TRUE(service->NotifyOffloadSuccess(
            writer, {OffloadTaskItem{.tenant_id = t, .key = key, .size = 1024}},
            {metadata}));
    }
};

TEST_F(FiniteSnapshotTest, HttpAllowlistAndAllReplicaStates) {
    Put(tenant, false);
    Disk(tenant);
    Put("default");
    const auto id = Id("phala.shared-cache-key.v1", key);
    Json::Value manifest;
    manifest["case_id"] = identity.case_id;
    manifest["epoch"] = identity.epoch;
    manifest["tenant_id"] = tenant;
    manifest["key_salt"] = identity.key_salt;
    manifest["max_snapshots"] = 2;
    manifest["max_duration_ms"] = 120000;
    manifest["max_response_bytes"] = 65536;
    manifest["max_total_logical_bytes"] = 1048576;
    manifest["keys"][0]["key_id"] = id;
    manifest["keys"][0]["key"] = key;
    const auto path = (root / "manifest.json").string();
    std::ofstream(path) << manifest;
    ASSERT_EQ(chmod(path.c_str(), 0600), 0);
    ASSERT_EQ(
        setenv("MOONCAKE_SHARED_CACHE_SNAPSHOT_MANIFEST", path.c_str(), 1), 0);
    const auto port = getFreeTcpPort();
    MasterAdminServer admin(port, false);
    ASSERT_TRUE(admin.Start());
    admin.SetRuntimeState(ha::MasterRuntimeState::kServing);
    admin.SetServiceDelegate(service);
    admin.SetServiceAvailable(true);
    const std::string url =
        "http://127.0.0.1:" + std::to_string(port) + "/batch_query_keys";
    auto get = [&](const std::string& query) {
        coro_http::coro_http_client client;
        auto result = client.get(url + query);
        return std::pair<int, std::string>{result.status,
                                           std::string(result.resp_body)};
    };
    const std::string query = "?finite_snapshot=1&tenant_id=" + tenant +
                              "&case_id=case1&epoch=epoch1&key_ids=" + id;
    EXPECT_EQ(get("?finite_snapshot=1&key_ids=" + id).first, 400);
    EXPECT_EQ(get(query + ",unknown&sample_id=invalid").first, 400);
    EXPECT_EQ(get(query + "," + id + "&sample_id=duplicate").first, 400);
    EXPECT_EQ(get(query + "&sample_id=raw&keys=" + key).first, 400);
    EXPECT_EQ(get(query + "&sample_id=raw&keys=").first, 400);
    EXPECT_EQ(get(query + "&sample_id=double&sample_id=again").first, 400);
    EXPECT_EQ(
        get(query + "&sample_id=long&extra=" + std::string(18000, 'x')).first,
        400);
    EXPECT_EQ(get("?finite_snapshot=1&tenant_id=default&case_id=case1&epoch="
                  "epoch1&sample_id=wrong&key_ids=" +
                  id)
                  .first,
              400);
    auto first = get(query + "&sample_id=before");
    ASSERT_EQ(first.first, 200) << first.second;
    Json::Value body;
    Json::Reader reader;
    ASSERT_TRUE(reader.parse(first.second, body));
    EXPECT_TRUE(body["master_multi_tenant_enabled"].asBool());
    EXPECT_EQ(body["requested_tenant_id"].asString(), tenant);
    EXPECT_EQ(body["effective_tenant_id"].asString(), tenant);
    auto item = body["objects"][0];
    EXPECT_TRUE(item["found"].asBool());
    EXPECT_EQ(item["writer_id"].asString(),
              Id("phala.shared-cache-owner.v1", UuidToString(writer)));
    EXPECT_EQ(item["pending"]["processing"].asUInt64(), 1);
    ASSERT_EQ(item["replicas"].size(), 2);
    bool processing = false, disk = false;
    for (const auto& replica : item["replicas"]) {
        if (replica["type"] == "MEMORY") {
            processing = replica["status"] == "PROCESSING";
            EXPECT_EQ(replica["segment_ids"][0].asString(),
                      Id("phala.shared-cache-segment.v1", segment.name));
        }
        if (replica["type"] == "LOCAL_DISK")
            disk = replica["status"] == "COMPLETE";
    }
    EXPECT_TRUE(processing);
    EXPECT_TRUE(disk);
    for (const auto& raw :
         {key, segment.name, UuidToString(writer), identity.key_salt,
          std::string("private-owner-endpoint")})
        EXPECT_EQ(first.second.find(raw), std::string::npos);
    EXPECT_EQ(get(query + "&sample_id=before").first, 400);
    EXPECT_EQ(get(query + "&sample_id=after").first, 200);
    EXPECT_EQ(get(query + "&sample_id=exhausted").first, 400);
    // Legacy/default observation remains available and keeps original shape.
    auto legacy = get("?keys=" + key);
    EXPECT_EQ(legacy.first, 200);
    EXPECT_NE(legacy.second.find("values"), std::string::npos);
    admin.Stop();
}

TEST_F(FiniteSnapshotTest, TenantRpcIsolationLeaseWriterAndNoSnapshotRenewal) {
    Put(tenant);
    Disk(tenant);
    Put("tenant-b");
    Put("default");
    const int port = getFreeTcpPort();
    coro_rpc::coro_rpc_server server(2, port, "127.0.0.1");
    RegisterRpcService(server, *service);
    auto started = server.async_start();
    ASSERT_FALSE(started.hasResult());
    const auto address = std::string("127.0.0.1:") + std::to_string(port);
    MasterClient client(writer, nullptr, tenant),
        other(writer, nullptr, "tenant-b"), legacy(writer);
    ASSERT_EQ(client.Connect(address), ErrorCode::OK);
    ASSERT_EQ(other.Connect(address), ErrorCode::OK);
    ASSERT_EQ(legacy.Connect(address), ErrorCode::OK);
    ASSERT_TRUE(service->GetReplicaList(
        key, tenant));  // grants an actual lease, MEMORY already exists
    EXPECT_FALSE(Snapshot(tenant)["objects"][0]["lease_expired"].asBool());
    auto active = client.BatchReplicaClear({key}, writer, segment.name);
    ASSERT_TRUE(active);
    EXPECT_TRUE(active->empty());
    std::this_thread::sleep_for(std::chrono::milliseconds(450));
    auto wrong = client.BatchReplicaClear({key}, generate_uuid(), segment.name);
    ASSERT_TRUE(wrong);
    EXPECT_TRUE(wrong->empty());
    auto before = Snapshot(tenant);
    ASSERT_TRUE(before["objects"][0]["lease_expired"].asBool());
    const auto promotions =
        MasterMetricManager::instance().get_promotion_admitted();
    auto cleared = client.BatchReplicaClear({key}, writer, segment.name);
    ASSERT_TRUE(cleared);
    EXPECT_EQ(*cleared, std::vector<std::string>{key});
    auto after = Snapshot(tenant);
    ASSERT_TRUE(after["objects"][0]["found"].asBool());
    ASSERT_EQ(after["objects"][0]["replicas"].size(), 1);
    EXPECT_EQ(after["objects"][0]["replicas"][0]["type"], "LOCAL_DISK");
    EXPECT_EQ(MasterMetricManager::instance().get_promotion_admitted(),
              promotions);
    EXPECT_EQ(Snapshot("tenant-b")["objects"][0]["replicas"][0]["type"],
              "MEMORY");
    EXPECT_EQ(Snapshot("default")["objects"][0]["replicas"][0]["type"],
              "MEMORY");
    auto default_clear = legacy.BatchReplicaClear({key}, writer, segment.name);
    ASSERT_TRUE(default_clear);
    EXPECT_EQ(default_clear->size(), 1);
    EXPECT_TRUE(Snapshot("tenant-b")["objects"][0]["found"].asBool());
    auto limited = Request(tenant);
    limited.max_total_logical_bytes = 1;
    EXPECT_FALSE(service->GetFiniteSnapshotForAdmin(limited));
    limited = Request(tenant);
    limited.deadline = std::chrono::steady_clock::now();
    EXPECT_FALSE(service->GetFiniteSnapshotForAdmin(limited));
    // A deliberate real read admits work; the RO snapshot must report the
    // actual task and leave it queued for its original disk owner.
    ASSERT_TRUE(service->GetReplicaList(key, tenant));
    auto pending = Snapshot(tenant);
    EXPECT_EQ(pending["objects"][0]["pending"]["promotion"].asUInt64(), 1);
    EXPECT_EQ(
        pending["objects"][0]["pending"]["dynamic_replication"].asUInt64(), 0);
    auto work_after = service->PromotionObjectHeartbeat(writer);
    ASSERT_TRUE(work_after);
    EXPECT_EQ(work_after->size(), 1);
    server.stop();
}

TEST_F(FiniteSnapshotTest, OldMasterDoesNotFallbackToDefaultTenant) {
    Put("default");
    const int port = getFreeTcpPort();
    coro_rpc::coro_rpc_server server(2, port, "127.0.0.1");
    server.register_handler<&WrappedMasterService::ServiceReady>(service.get());
    server.register_handler<&WrappedMasterService::BatchReplicaClear>(
        service.get());
    auto started = server.async_start();
    ASSERT_FALSE(started.hasResult());
    MasterClient client(writer, nullptr, tenant);
    ASSERT_EQ(client.Connect("127.0.0.1:" + std::to_string(port)),
              ErrorCode::OK);
    auto result = client.BatchReplicaClear({key}, writer, segment.name);
    EXPECT_FALSE(result);
    EXPECT_TRUE(Snapshot("default")["objects"][0]["found"].asBool());
    server.stop();
}

TEST_F(FiniteSnapshotTest, SingleTenantModeIsExplicitAndRejectsNonDefault) {
    WrappedMasterServiceConfig config;
    config.enable_metric_reporting = false;
    config.default_kv_lease_ttl = 1;
    WrappedMasterService single(config);
    ASSERT_TRUE(single.MountSegment(segment, writer));
    ReplicateConfig replication;
    replication.replica_num = 1;
    ASSERT_TRUE(single.PutStart(writer, key, 1024, replication, "default"));
    ASSERT_TRUE(single.PutEnd(writer, ObjectMeta{key, std::nullopt},
                              ReplicaType::MEMORY, "default"));
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    EXPECT_FALSE(single.GetFiniteSnapshotForAdmin(Request(tenant)));
    auto clear =
        single.BatchReplicaClearForTenant({key}, writer, segment.name, tenant);
    EXPECT_FALSE(clear);
    auto output = single.GetFiniteSnapshotForAdmin(Request("default"));
    ASSERT_TRUE(output);
    Json::Value body;
    Json::Reader reader;
    ASSERT_TRUE(reader.parse(*output, body));
    EXPECT_FALSE(body["master_multi_tenant_enabled"].asBool());
    EXPECT_EQ(body["requested_tenant_id"].asString(), "default");
    EXPECT_EQ(body["effective_tenant_id"].asString(), "default");
    ASSERT_TRUE(body["objects"][0]["found"].asBool());
    auto cleared = single.BatchReplicaClear({key}, writer, segment.name);
    ASSERT_TRUE(cleared);
    EXPECT_EQ(*cleared, std::vector<std::string>{key});
}
}  // namespace mooncake::test

int main(int argc, char** argv) {
    google::InitGoogleLogging(argv[0]);
    FLAGS_logtostderr = true;
    ::testing::InitGoogleTest(&argc, argv);
    auto result = RUN_ALL_TESTS();
    google::ShutdownGoogleLogging();
    return result;
}
