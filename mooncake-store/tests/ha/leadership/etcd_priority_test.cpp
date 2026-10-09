#include <gtest/gtest.h>
#include <unistd.h>

#include <chrono>
#include <cstdlib>

#include "etcd_helper.h"
#include "ha/leadership/backends/etcd/etcd_leader_coordinator.h"

namespace mooncake::ha {
namespace {
using backends::etcd::EtcdLeaderCoordinator;

class EtcdPriorityTest : public ::testing::Test {
 protected:
    void SetUp() override {
        const char* endpoint = std::getenv("MOONCAKE_TEST_ETCD_ENDPOINTS");
        if (!endpoint) GTEST_SKIP() << "requires isolated etcd";
        spec_.type = HABackendType::ETCD;
        spec_.connstring = endpoint;
        spec_.cluster_namespace = "priority-test-" + std::to_string(getpid()) + "-" +
            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
        ASSERT_EQ(ErrorCode::OK, EtcdHelper::ConnectToEtcdStoreClient(spec_.connstring));
    }

    HABackendSpec Spec(uint32_t priority) {
        auto spec = spec_;
        spec.candidate_priority = priority;
        return spec;
    }
    HABackendSpec spec_;
};

TEST_F(EtcdPriorityTest, LowestEligibleWinsIndependentOfRegistrationOrder) {
    EtcdLeaderCoordinator high(Spec(102)), low(Spec(101));
    ASSERT_EQ(ErrorCode::OK, high.Connect());
    ASSERT_EQ(ErrorCode::OK, low.Connect());
    ASSERT_EQ(ErrorCode::OK, high.UpdateCandidateEligibility(true));
    ASSERT_EQ(ErrorCode::OK, low.UpdateCandidateEligibility(true));
    auto blocked = high.TryAcquireLeadership("node102:50051");
    ASSERT_TRUE(blocked);
    EXPECT_EQ(AcquireLeadershipStatus::CONTENDED, blocked->status);
    auto winner = low.TryAcquireLeadership("node101:50051");
    ASSERT_TRUE(winner);
    ASSERT_TRUE(winner->session);
    EXPECT_EQ(AcquireLeadershipStatus::ACQUIRED, winner->status);
    ASSERT_EQ(ErrorCode::OK, low.ReleaseLeadership(*winner->session));
    ASSERT_EQ(ErrorCode::OK, low.UpdateCandidateEligibility(false));
    auto next = high.TryAcquireLeadership("node102:50051");
    ASSERT_TRUE(next);
    ASSERT_TRUE(next->session);
    EXPECT_EQ(AcquireLeadershipStatus::ACQUIRED, next->status);
    EXPECT_EQ(ErrorCode::OK, high.ReleaseLeadership(*next->session));
}

TEST_F(EtcdPriorityTest, RecoveringCandidateCannotAcquireOrBlockReadyCandidate) {
    EtcdLeaderCoordinator low(Spec(101)), high(Spec(102));
    ASSERT_EQ(ErrorCode::OK, low.Connect());
    ASSERT_EQ(ErrorCode::OK, high.Connect());
    auto not_ready = low.TryAcquireLeadership("node101:50051");
    ASSERT_TRUE(not_ready);
    EXPECT_EQ(AcquireLeadershipStatus::CONTENDED, not_ready->status);
    ASSERT_EQ(ErrorCode::OK, low.UpdateCandidateEligibility(true));
    ASSERT_EQ(ErrorCode::OK, low.UpdateCandidateEligibility(false));
    ASSERT_EQ(ErrorCode::OK, high.UpdateCandidateEligibility(true));
    auto winner = high.TryAcquireLeadership("node102:50051");
    ASSERT_TRUE(winner);
    ASSERT_TRUE(winner->session);
    EXPECT_EQ(ErrorCode::OK, high.ReleaseLeadership(*winner->session));
}

TEST_F(EtcdPriorityTest, ReturningLowerPriorityDoesNotPreemptHealthyLeader) {
    EtcdLeaderCoordinator high(Spec(102)), low(Spec(101));
    ASSERT_EQ(ErrorCode::OK, high.Connect());
    ASSERT_EQ(ErrorCode::OK, low.Connect());
    ASSERT_EQ(ErrorCode::OK, high.UpdateCandidateEligibility(true));
    auto winner = high.TryAcquireLeadership("node102:50051");
    ASSERT_TRUE(winner);
    ASSERT_TRUE(winner->session);
    ASSERT_EQ(ErrorCode::OK, low.UpdateCandidateEligibility(true));
    auto blocked = low.TryAcquireLeadership("node101:50051");
    ASSERT_TRUE(blocked);
    EXPECT_EQ(AcquireLeadershipStatus::CONTENDED, blocked->status);
    ASSERT_TRUE(blocked->observed_view);
    EXPECT_EQ("node102:50051", blocked->observed_view->leader_address);
    EXPECT_EQ(ErrorCode::OK, high.ReleaseLeadership(*winner->session));
}

TEST_F(EtcdPriorityTest, RevokedRegistrationCannotCampaignWithStaleIdentity) {
    const auto prefix = spec_.cluster_namespace + "/candidates/";
    const auto key = prefix + "0000000101";
    int64_t handle = 0;
    EtcdLeaseId candidate_lease = 0, leader_lease = 0;
    EtcdRevisionId candidate_revision = 0, revision = 0;
    ASSERT_EQ(ErrorCode::OK, EtcdHelper::AcquireMaintenanceSession(
        key, 10, handle, candidate_lease, candidate_revision));
    ASSERT_EQ(ErrorCode::OK, EtcdHelper::CloseMaintenanceSession(handle));
    ASSERT_EQ(ErrorCode::OK, EtcdHelper::GrantLease(10, leader_lease));
    EXPECT_EQ(ErrorCode::ETCD_TRANSACTION_FAIL,
        EtcdHelper::CreateWithLeaseIfFirstCandidate(
            spec_.cluster_namespace + "/view", "node101:50051", leader_lease,
            prefix, key, candidate_lease, candidate_revision, revision));
    EXPECT_EQ(ErrorCode::OK, EtcdHelper::RevokeLease(leader_lease));
}
}  // namespace
}  // namespace mooncake::ha
