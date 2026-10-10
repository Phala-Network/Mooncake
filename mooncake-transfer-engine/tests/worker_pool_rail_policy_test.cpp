// Copyright 2026 KVCache.AI
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy at http://www.apache.org/licenses/LICENSE-2.0

#include <arpa/inet.h>
#include <gtest/gtest.h>

#include "config.h"
#include "cuda_alike.h"
#include "rdma_test_peers.h"
#include "transport/rdma_transport/worker_pool.h"

namespace mooncake {
class WorkerPoolTestPeer {
   public:
    static void stop(WorkerPool &pool) {
        pool.workers_running_.store(false);
        pool.cond_var_.notify_all();
        for (auto &thread : pool.worker_thread_) thread.join();
    }
    static void pause(WorkerPool &pool, const std::string &path) {
        pool.markRailFailed(path, true);
    }
    static void redispatch(WorkerPool &pool,
                           std::vector<Transport::Slice *> &slices,
                           bool local = false) {
        pool.redispatch(slices, 0, local);
    }
    static bool handoff(WorkerPool &pool, Transport::Slice &slice) {
        return pool.tryHandoffToAnotherLocalWorker(&slice);
    }
    static bool alternative(WorkerPool &pool, Transport::Slice &slice,
                            const std::string &failed) {
        return pool.hasAvailablePeerRailAlternative(&slice, failed);
    }
    static void expire(WorkerPool &pool, const std::string &path) {
        pool.rail_states_[path].pause_until_ns = 1;
    }
    static void clear(WorkerPool &pool) {
        for (auto &queue : pool.slice_queue_) queue.clear();
        for (auto &queue : pool.collective_slice_queue_) queue.clear();
    }
};
}  // namespace mooncake

namespace {
using namespace mooncake;

// Exercises actual submit/redispatch with workers stopped and local metadata.
// No verbs device, QP, server listener, or network request is needed.
class WorkerPoolRailPolicyTest : public ::testing::Test {
   protected:
    std::shared_ptr<TransferMetadata> metadata;
    std::unique_ptr<RdmaTransport> transport;
    std::shared_ptr<RdmaContext> context;
    std::unique_ptr<WorkerPool> pool;
    std::shared_ptr<TransferMetadata::SegmentDesc> desc;
    std::vector<std::shared_ptr<RdmaContext>> alternates;
    std::vector<std::shared_ptr<WorkerPool>> alternate_pools;
    RdmaRailGroups old_groups;
    bool old_affinity;
    bool old_hca_affinity;
    std::unordered_map<std::string, std::vector<std::string>> old_peer_affinity;
    int old_log_level;

    void SetUp() override {
        old_affinity = globalConfig().enable_dest_device_affinity;
        old_hca_affinity = globalConfig().enable_hca_peer_affinity;
        old_peer_affinity = globalConfig().nic_peer_affinity;
        globalConfig().enable_hca_peer_affinity = false;
        globalConfig().enable_dest_device_affinity = true;
        old_groups = globalConfig().rdma_rail_groups;
        ASSERT_TRUE(globalConfig().rdma_rail_groups.configure(
            "10.80.20.0/24;10.80.21.0/24"));
        old_log_level = FLAGS_minloglevel;
        FLAGS_minloglevel = google::GLOG_FATAL;
        metadata = std::make_shared<TransferMetadata>(P2PHANDSHAKE);
        transport = std::make_unique<RdmaTransport>();
        RdmaTransportTestPeer::bindMetadata(*transport, metadata, "test:1234");
        context = std::make_shared<RdmaContext>(*transport, "rail20_a");
        // Nonexistent device initializes endpoint storage without opening HW.
        context->construct();
        ibv_gid gid{};
        ASSERT_EQ(inet_pton(AF_INET6, "::ffff:10.80.20.1", gid.raw), 1);
        RdmaContextTestPeer::seedAutoGidState(*context, nullptr, 1, 0, gid, 3);
        RdmaTransportTestPeer::addContext(*transport, context);
        pool = std::make_unique<WorkerPool>(*context);
        WorkerPoolTestPeer::stop(*pool);

        desc = std::make_shared<TransferMetadata::SegmentDesc>();
        desc->name = "test:1234";
        desc->protocol = "rdma";
        for (auto [name, address] :
             {std::pair{"rail20_a",
                        "00:00:00:00:00:00:00:00:00:00:ff:ff:0a:50:14:09"},
              std::pair{"rail21_b",
                        "00:00:00:00:00:00:00:00:00:00:ff:ff:0a:50:15:09"},
              std::pair{"rail20_c",
                        "00:00:00:00:00:00:00:00:00:00:ff:ff:0a:50:14:0a"}}) {
            TransferMetadata::DeviceDesc device;
            device.name = name;
            device.gid = address;
            desc->devices.push_back(device);
        }
        TransferMetadata::BufferDesc buffer;
        buffer.name = "cpu:0";
        buffer.addr = 4096;
        buffer.length = 8192;
        buffer.lkey = {1, 2, 3};
        buffer.rkey = {4, 5, 6};
        desc->buffers.push_back(buffer);
        ASSERT_EQ(desc->topology.parse(
                      R"({"cpu:0":[["rail20_a","rail21_b","rail20_c"],[]]})"),
                  0);
        auto published = desc;
        ASSERT_EQ(metadata->addLocalSegment(LOCAL_SEGMENT_ID, desc->name,
                                            std::move(published)),
                  0);
    }
    void TearDown() override {
        WorkerPoolTestPeer::clear(*pool);
        for (auto &other : alternate_pools) WorkerPoolTestPeer::clear(*other);
        for (auto &other : alternates)
            RdmaContextTestPeer::bindWorkerPool(*other, nullptr);
        alternate_pools.clear();
        alternates.clear();
        pool.reset();
        context.reset();
        transport.reset();
        metadata.reset();
        globalConfig().enable_dest_device_affinity = old_affinity;
        globalConfig().enable_hca_peer_affinity = old_hca_affinity;
        globalConfig().nic_peer_affinity = old_peer_affinity;
        globalConfig().rdma_rail_groups = old_groups;
        FLAGS_minloglevel = old_log_level;
    }
    void run(bool redispatch, const std::string &expected) {
        Transport::TransferTask task;
        task.batch_id = transport->allocateBatchID(1);
        Transport::Slice slice{};
        slice.task = &task;
        slice.status = Transport::Slice::PENDING;
        slice.target_id = LOCAL_SEGMENT_ID;
        slice.length = 64;
        slice.rdma.dest_addr = 4096;
        slice.rdma.retry_cnt = redispatch ? 1 : 0;
        slice.rdma.max_retry_cnt = 8;
        std::vector<Transport::Slice *> slices{&slice};
        if (redispatch)
            WorkerPoolTestPeer::redispatch(*pool, slices);
        else
            EXPECT_EQ(pool->submitPostSend(slices), 0);
        if (expected.empty()) {
            EXPECT_EQ(slice.status, Transport::Slice::FAILED);
            EXPECT_EQ(task.failed_slice_count, 1u);
        } else {
            EXPECT_EQ(slice.status, Transport::Slice::PENDING);
            EXPECT_EQ(task.failed_slice_count, 0u);
            EXPECT_EQ(slice.peer_nic_path,
                      MakeNicPath(desc->nicPathServerName(), expected));
            for (size_t i = 0; i < desc->devices.size(); ++i) {
                if (desc->devices[i].name == expected)
                    EXPECT_EQ(slice.rdma.dest_rkey, desc->buffers[0].rkey[i]);
            }
        }
        WorkerPoolTestPeer::clear(*pool);
        EXPECT_TRUE(transport->freeBatchID(task.batch_id).ok());
    }
    void addAlternate(const std::string &name, const std::string &address) {
        auto other = std::make_shared<RdmaContext>(*transport, name);
        other->construct();
        other->set_active(true);
        ibv_gid gid{};
        ASSERT_EQ(inet_pton(AF_INET6, address.c_str(), gid.raw), 1);
        RdmaContextTestPeer::seedAutoGidState(*other, nullptr, 1, 0, gid, 3);
        auto other_pool = std::make_shared<WorkerPool>(*other);
        WorkerPoolTestPeer::stop(*other_pool);
        RdmaContextTestPeer::bindWorkerPool(*other, other_pool);
        RdmaTransportTestPeer::addContext(*transport, other);
        alternates.push_back(other);
        alternate_pools.push_back(other_pool);
    }
    void runHandoff(bool redispatch, bool expected,
                    unsigned expected_lkey = 3) {
        Transport::TransferTask task;
        task.batch_id = transport->allocateBatchID(1);
        Transport::Slice slice{};
        slice.status = Transport::Slice::PENDING;
        slice.task = &task;
        slice.target_id = LOCAL_SEGMENT_ID;
        slice.length = 64;
        slice.source_addr = reinterpret_cast<void *>(4096);
        slice.rdma.dest_addr = 4096;
        slice.rdma.dest_rkey = 4;
        slice.rdma.source_lkey = 1;
        slice.rdma.retry_cnt = 1;
        slice.rdma.max_retry_cnt = 8;
        slice.peer_nic_path = MakeNicPath(desc->name, "rail20_a");
        context->set_active(false);
        if (redispatch) {
            std::vector<Transport::Slice *> slices{&slice};
            WorkerPoolTestPeer::redispatch(*pool, slices, true);
            EXPECT_EQ(slice.status, expected ? Transport::Slice::PENDING
                                             : Transport::Slice::FAILED);
            EXPECT_EQ(task.failed_slice_count, expected ? 0u : 1u);
        } else {
            EXPECT_EQ(WorkerPoolTestPeer::handoff(*pool, slice), expected);
        }
        EXPECT_EQ(slice.rdma.dest_rkey, 4u);
        EXPECT_EQ(slice.peer_nic_path, MakeNicPath(desc->name, "rail20_a"));
        EXPECT_EQ(slice.rdma.source_lkey, expected ? expected_lkey : 1u);
        for (auto &other : alternate_pools) WorkerPoolTestPeer::clear(*other);
        EXPECT_TRUE(transport->freeBatchID(task.batch_id).ok());
    }
};

TEST_F(WorkerPoolRailPolicyTest, HealthySameNameIsPreserved) {
    run(false, "rail20_a");
}
TEST_F(WorkerPoolRailPolicyTest, SubmitPausedPeerUsesSameRailAlternative) {
    WorkerPoolTestPeer::pause(*pool, MakeNicPath(desc->name, "rail20_a"));
    run(false, "rail20_c");
}
TEST_F(WorkerPoolRailPolicyTest, RedispatchPausedPeerUsesSameRailAlternative) {
    WorkerPoolTestPeer::pause(*pool, MakeNicPath(desc->name, "rail20_a"));
    run(true, "rail20_c");
}
TEST_F(WorkerPoolRailPolicyTest, DisabledPolicyRetainsFullMeshFallback) {
    ASSERT_TRUE(globalConfig().rdma_rail_groups.configure(nullptr));
    WorkerPoolTestPeer::pause(*pool, MakeNicPath(desc->name, "rail20_a"));
    run(false, "rail21_b");
    run(true, "rail21_b");
}
TEST_F(WorkerPoolRailPolicyTest, DisabledTopologyUsesHealthySameRail) {
    desc->topology.disableDevice("rail20_a");
    run(false, "rail20_c");
    run(true, "rail20_c");
}
TEST_F(WorkerPoolRailPolicyTest, PausedFallbackCannotUseDisabledSameRail) {
    desc->topology.disableDevice("rail20_c");
    WorkerPoolTestPeer::pause(*pool, MakeNicPath(desc->name, "rail20_a"));
    run(false, "");
    run(true, "");
}
TEST_F(WorkerPoolRailPolicyTest, HeterogeneousNamesStillUseGidGroup) {
    desc->devices[0].name = "hostB_x";
    desc->devices[1].name = "hostB_y";
    desc->devices[2].name = "hostB_z";
    ASSERT_EQ(desc->topology.parse(
                  R"({"cpu:0":[["hostB_x","hostB_y","hostB_z"],[]]})"),
              0);
    WorkerPoolTestPeer::pause(*pool, MakeNicPath(desc->name, "hostB_x"));
    run(true, "hostB_z");
}
TEST_F(WorkerPoolRailPolicyTest, SecondRailCanUseItsAlternative) {
    ibv_gid gid{};
    ASSERT_EQ(inet_pton(AF_INET6, "::ffff:10.80.21.1", gid.raw), 1);
    RdmaContextTestPeer::seedAutoGidState(*context, nullptr, 1, 0, gid, 3);
    desc->devices[2].gid = "00:00:00:00:00:00:00:00:00:00:ff:ff:0a:50:15:0a";
    WorkerPoolTestPeer::pause(*pool, MakeNicPath(desc->name, "rail21_b"));
    run(false, "rail20_c");
    run(true, "rail20_c");
}
TEST_F(WorkerPoolRailPolicyTest, UnknownLocalGidFailsClosed) {
    ibv_gid gid{};
    RdmaContextTestPeer::seedAutoGidState(*context, nullptr, 1, 0, gid, 3);
    run(false, "");
    run(true, "");
}
TEST_F(WorkerPoolRailPolicyTest, UnknownPeerGidsFailClosed) {
    desc->devices[0].gid.clear();
    desc->devices[2].gid = "malformed";
    run(false, "");
    run(true, "");
}
TEST_F(WorkerPoolRailPolicyTest, AlternativeCheckExcludesOtherRail) {
    Transport::Slice slice;
    slice.target_id = LOCAL_SEGMENT_ID;
    slice.length = 64;
    slice.rdma.dest_addr = 4096;
    auto failed = MakeNicPath(desc->name, "rail20_a");
    EXPECT_TRUE(WorkerPoolTestPeer::alternative(*pool, slice, failed));
    desc->topology.disableDevice("rail20_c");
    EXPECT_FALSE(WorkerPoolTestPeer::alternative(*pool, slice, failed));
}
TEST_F(WorkerPoolRailPolicyTest, PauseExpiryAllowsFreshSameNameSubmission) {
    auto path = MakeNicPath(desc->name, "rail20_a");
    WorkerPoolTestPeer::pause(*pool, path);
    run(false, "rail20_c");
    WorkerPoolTestPeer::expire(*pool, path);
    run(false, "rail20_a");
}
TEST_F(WorkerPoolRailPolicyTest, LocalHandoffUsesSameRailSource) {
    addAlternate("rail21_b", "::ffff:10.80.21.1");
    addAlternate("rail20_c", "::ffff:10.80.20.2");
    runHandoff(false, true);
    runHandoff(true, true);
}
TEST_F(WorkerPoolRailPolicyTest, LocalHandoffRejectsOnlyOtherRail) {
    addAlternate("rail21_b", "::ffff:10.80.21.1");
    runHandoff(false, false);
    runHandoff(true, false);
}
TEST_F(WorkerPoolRailPolicyTest, DisabledPolicyRetainsLocalHandoff) {
    ASSERT_TRUE(globalConfig().rdma_rail_groups.configure(nullptr));
    addAlternate("rail21_b", "::ffff:10.80.21.1");
    addAlternate("rail20_c", "::ffff:10.80.20.2");
    runHandoff(false, true, 2);
    runHandoff(true, true, 2);
}
TEST_F(WorkerPoolRailPolicyTest, LocalHandoffHonorsAlternatePoolPause) {
    addAlternate("rail21_b", "::ffff:10.80.21.1");
    addAlternate("rail20_c", "::ffff:10.80.20.2");
    WorkerPoolTestPeer::pause(*alternate_pools[1],
                              MakeNicPath(desc->name, "rail20_a"));
    runHandoff(true, false);
}
TEST_F(WorkerPoolRailPolicyTest, AllEightSameRailHcasRemainAvailable) {
    for (auto suffix : {"0b", "0c", "0d", "0e", "0f", "10"}) {
        TransferMetadata::DeviceDesc device;
        device.name = std::string("extra_") + suffix;
        device.gid =
            std::string("00:00:00:00:00:00:00:00:00:00:ff:ff:0a:50:14:") +
            suffix;
        desc->devices.push_back(device);
        desc->buffers[0].rkey.push_back(4 + desc->devices.size() - 1);
        desc->buffers[0].lkey.push_back(desc->devices.size());
    }
    Json::Value matrix, list(Json::arrayValue), entry(Json::arrayValue);
    for (const auto &device : desc->devices) list.append(device.name);
    entry.append(list);
    entry.append(Json::Value(Json::arrayValue));
    matrix["cpu:0"] = entry;
    ASSERT_EQ(desc->topology.parse(matrix.toStyledString()), 0);
    for (const auto &device : desc->devices) {
        if (device.name == "rail21_b") continue;
        run(false, device.name);
        WorkerPoolTestPeer::pause(*pool, MakeNicPath(desc->name, device.name));
    }
    run(false, "");
    run(true, "");
}
TEST_F(WorkerPoolRailPolicyTest, GpuExplicitAffinityStillRestrictsCandidates) {
    auto &config = globalConfig();
    config.enable_dest_device_affinity = false;
    config.enable_hca_peer_affinity = true;
    config.nic_peer_affinity = {{"rail20_a", {"rail20_a", "rail21_b"}}};
    const std::string location = GPU_PREFIX + "0";
    desc->buffers[0].name = location;
    ASSERT_EQ(desc->topology.parse(
                  "{\"" + location +
                  "\":[[\"rail20_a\",\"rail21_b\",\"rail20_c\"],[]]}"),
              0);
    WorkerPoolTestPeer::pause(*pool, MakeNicPath(desc->name, "rail20_a"));
    run(true, "");
    config.nic_peer_affinity["rail20_a"] = {"rail20_a", "rail20_c"};
    ASSERT_EQ(desc->topology.parse(
                  "{\"" + location +
                  "\":[[\"rail20_a\",\"rail21_b\",\"rail20_c\"],[]]}"),
              0);
    run(true, "rail20_c");
}
TEST_F(WorkerPoolRailPolicyTest, SegmentedCpuAndWildcardLocations) {
    desc->buffers[0].name = "segments:4096:0,1";
    WorkerPoolTestPeer::pause(*pool, MakeNicPath(desc->name, "rail20_a"));
    run(false, "rail20_c");
    desc->buffers[0].name = "cpu:99";
    run(true, "rail20_c");
}
TEST_F(WorkerPoolRailPolicyTest, MissingAlternateRkeyFailsWithoutCrossRail) {
    desc->buffers[0].rkey.pop_back();
    WorkerPoolTestPeer::pause(*pool, MakeNicPath(desc->name, "rail20_a"));
    run(false, "");
    run(true, "");
}
TEST_F(WorkerPoolRailPolicyTest, RdmaServerAliasStillUsesGidGroups) {
    desc->rdma_server_name = "rdma-alias:4321";
    WorkerPoolTestPeer::pause(*pool,
                              MakeNicPath(desc->rdma_server_name, "rail20_a"));
    run(false, "rail20_c");
    run(true, "rail20_c");
}

constexpr auto kGid20 = "00:00:00:00:00:00:00:00:00:00:ff:ff:0a:50:14:01";
constexpr auto kGid20Other = "00:00:00:00:00:00:00:00:00:00:ff:ff:0a:50:14:fe";
constexpr auto kGid21 = "00:00:00:00:00:00:00:00:00:00:ff:ff:0a:50:15:01";

TEST(RdmaRailGroupsTest, UnsetPreservesExistingBehavior) {
    RdmaRailGroups policy;
    ASSERT_TRUE(policy.configure(nullptr));
    EXPECT_FALSE(policy.enabled());
    EXPECT_TRUE(policy.allows("unknown", "unknown"));
    EXPECT_TRUE(policy.allows(kGid20, kGid21));
}
TEST(RdmaRailGroupsTest, Ipv4MappedGidsMatchExplicitGroups) {
    RdmaRailGroups policy;
    ASSERT_TRUE(policy.configure("10.80.20.0/24;10.80.21.0/24"));
    EXPECT_TRUE(policy.allows(kGid20, kGid20Other));
    EXPECT_FALSE(policy.allows(kGid20, kGid21));
    EXPECT_FALSE(policy.allows("", kGid20));
    EXPECT_FALSE(policy.allows(kGid20, "::ffff:10.80.20.1"));
    EXPECT_FALSE(policy.allows(kGid20, std::string(kGid20) + ":00"));
}
TEST(RdmaRailGroupsTest, Ipv6PrefixesAndNonByteBoundary) {
    RdmaRailGroups policy;
    ASSERT_TRUE(policy.configure("2001:db8::/65;2001:db8:1::/64"));
    EXPECT_TRUE(
        policy.allows("20:01:0d:b8:00:00:00:00:00:00:00:00:00:00:00:01",
                      "20:01:0d:b8:00:00:00:00:7f:00:00:00:00:00:00:01"));
    EXPECT_FALSE(
        policy.allows("20:01:0d:b8:00:00:00:00:00:00:00:00:00:00:00:01",
                      "20:01:0d:b8:00:00:00:00:80:00:00:00:00:00:00:01"));
}
TEST(RdmaRailGroupsTest, RejectsAmbiguousOrMalformedConfiguration) {
    for (const char *value :
         {"", ";", "10.80.20.0/24;", "10.80.20.0/24;;10.80.21.0/24",
          "10.80.20.0", "10.80.20.0/33", "::/129", "10.80.20.0/-1",
          "10.80.20.0/24x", "10.80.20.0/24;10.80.20.0/24",
          "10.80.20.0/24;10.80.20.128/25",
          "::ffff:10.80.20.0/120;10.80.20.0/24"}) {
        RdmaRailGroups policy;
        EXPECT_FALSE(policy.configure(value)) << value;
    }
}
TEST(RdmaRailGroupsTest, InvalidConfigurationStopsInitialization) {
    ASSERT_DEATH(
        {
            setenv("MC_RDMA_RAIL_GROUPS", "10.80.20.0/24;10.80.20.0/25", 1);
            GlobalConfig config;
            loadGlobalConfig(config);
        },
        "Invalid MC_RDMA_RAIL_GROUPS");
}
}  // namespace
