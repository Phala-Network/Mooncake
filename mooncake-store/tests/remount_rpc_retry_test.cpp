#include <gtest/gtest.h>

#include <chrono>
#include <future>
#include <memory>
#include <shared_mutex>
#include <string>
#include <thread>
#include <vector>

#include <ylt/coro_rpc/coro_rpc_client.hpp>
#include <ylt/coro_rpc/coro_rpc_server.hpp>

#include "master_metric_manager.h"
#include "rpc_service.h"
#include "types.h"
#include "utils.h"

namespace mooncake::test {

class RemountRpcRetryTest : public ::testing::Test {
   protected:
    void SetUp() override {
        const auto ports = getFreeTcpPorts(1);
        ASSERT_EQ(ports.size(), 1u);
        const std::string port = std::to_string(ports.front());
        WrappedMasterServiceConfig config;
        config.default_kv_lease_ttl = DEFAULT_DEFAULT_KV_LEASE_TTL;
        config.enable_metric_reporting = false;
        wrapped_ = std::make_unique<WrappedMasterService>(config);
        // Both connections must use the same io thread. A direct call to
        // MasterService from a different thread would miss this regression.
        server_ = std::make_unique<coro_rpc::coro_rpc_server>(
            1, ports.front(), "127.0.0.1", std::chrono::seconds(0), true);
        RegisterRpcService(*server_, *wrapped_);
        ASSERT_FALSE(server_->async_start().hasResult());
        for (auto* client : {&remount_client_, &ping_client_}) {
            ASSERT_FALSE(async_simple::coro::syncAwait(
                client->connect("127.0.0.1", port)));
        }
    }

    void TearDown() override {
        if (server_) {
            server_->stop();
        }
    }

    void ExpectPingWhileRemountRetries(bool exclusive) {
        auto& service = wrapped_->master_service_;
        const UUID healthy_id = generate_uuid();
        const UUID recovering_id = generate_uuid();
        ASSERT_TRUE(service.ReMountSegment({}, healthy_id).has_value());
        std::unique_lock<std::shared_mutex> writer(service.snapshot_mutex_,
                                                   std::defer_lock);
        std::shared_lock<std::shared_mutex> reader(service.snapshot_mutex_,
                                                   std::defer_lock);
        if (exclusive) {
            writer.lock();
        } else {
            reader.lock();
        }

        const auto requests_before =
            MasterMetricManager::instance().get_remount_segment_requests();
        auto remount = std::async(std::launch::async, [&] {
            return async_simple::coro::syncAwait(
                remount_client_.call<&WrappedMasterService::ReMountSegment>(
                    std::vector<Segment>{}, recovering_id));
        });
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::seconds(1);
        while (MasterMetricManager::instance().get_remount_segment_requests() ==
                   requests_before &&
               std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        const bool handler_entered =
            MasterMetricManager::instance().get_remount_segment_requests() >
            requests_before;
        auto ping = std::async(std::launch::async, [&] {
            return async_simple::coro::syncAwait(
                ping_client_.call<&WrappedMasterService::Ping>(healthy_id));
        });
        const bool remount_rejected =
            remount.wait_for(std::chrono::seconds(1)) ==
            std::future_status::ready;
        const bool ping_completed =
            ping.wait_for(std::chrono::seconds(1)) == std::future_status::ready;
        // Release before assertions/future destruction, so the unpatched
        // baseline fails deterministically instead of leaving a held handler.
        if (exclusive) {
            writer.unlock();
        } else {
            reader.unlock();
        }

        EXPECT_TRUE(handler_entered);
        EXPECT_TRUE(remount_rejected);
        EXPECT_TRUE(ping_completed);
        const auto remount_result = remount.get();
        ASSERT_TRUE(remount_result.has_value());
        EXPECT_FALSE(remount_result->has_value());
        if (!remount_result->has_value()) {
            EXPECT_EQ(remount_result->error(),
                      ErrorCode::UNAVAILABLE_IN_CURRENT_STATUS);
        }
        const auto ping_result = ping.get();
        ASSERT_TRUE(ping_result.has_value());
        ASSERT_TRUE(ping_result->has_value());
        EXPECT_EQ(ping_result->value().client_status, ClientStatus::OK);

        const auto before_retry = service.Ping(recovering_id);
        ASSERT_TRUE(before_retry.has_value());
        if (remount_rejected) {
            EXPECT_EQ(before_retry->client_status, ClientStatus::NEED_REMOUNT);
        }
        // Retry the real RPC on the same connection once the barrier clears.
        const auto retry = async_simple::coro::syncAwait(
            remount_client_.call<&WrappedMasterService::ReMountSegment>(
                std::vector<Segment>{}, recovering_id));
        ASSERT_TRUE(retry.has_value());
        ASSERT_TRUE(retry->has_value());
        const auto recovered = service.Ping(recovering_id);
        ASSERT_TRUE(recovered.has_value());
        EXPECT_EQ(recovered->client_status, ClientStatus::OK);
    }

    std::unique_ptr<WrappedMasterService> wrapped_;
    std::unique_ptr<coro_rpc::coro_rpc_server> server_;
    coro_rpc::coro_rpc_client remount_client_;
    coro_rpc::coro_rpc_client ping_client_;
};

TEST_F(RemountRpcRetryTest, PingContinuesWhileCleanupBlocksRemountAdmission) {
    ExpectPingWhileRemountRetries(/*exclusive=*/false);
}

TEST_F(RemountRpcRetryTest, PingContinuesWhileSnapshotBlocksRemountAdmission) {
    ExpectPingWhileRemountRetries(/*exclusive=*/true);
}

}  // namespace mooncake::test
