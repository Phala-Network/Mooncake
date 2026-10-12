// Control-plane-only probe against an isolated actual-image master.
// Synthetic addresses are never read or written. Byte correctness is verified
// separately by installed-sdk-probe.py with real allocations and SSD files.
#include "master_client.h"
#include <glog/logging.h>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <thread>
using namespace mooncake;
void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
int main(int argc, char** argv) {
    google::InitGoogleLogging("promotion-rpc-fencing-probe");
    require(argc == 2, "master address required");
    const auto holder = generate_uuid();
    MasterClient client(holder);
    require(client.Connect(argv[1]) == ErrorCode::OK, "connect");
    Segment segment;
    segment.id = generate_uuid();
    segment.name = "fencing-probe-synthetic";
    segment.te_endpoint = segment.name;
    segment.base = 0x100000000;
    segment.size = 16 * 1024 * 1024;
    require(bool(client.MountSegment(segment)), "mount memory");
    require(bool(client.MountLocalDiskSegment(holder, true)), "mount disk");
    const std::string key = "isolated-wire-fencing";
    StorageObjectMetadata meta{0, 0, int64_t(key.size()), 1024, segment.name};
    require(bool(client.NotifyOffloadSuccess(
                holder, std::vector<OffloadTaskItem>{{"default", key, 1024}},
                {meta})),
            "inject disk");
    require(bool(client.GetReplicaList(key)), "admit old");
    auto old = client.PromotionObjectHeartbeatV2(holder);
    require(old && old->size() == 1, "capture old UUID");
    const auto old_id = old->front().task_id;
    require(bool(client.PromotionAllocStartV2(holder, key, "default", 1024,
                                              {segment.name}, old_id)),
            "allocate old");
    UUID fresh_id{};
    bool fresh_found = false;
    const auto end =
        std::chrono::steady_clock::now() + std::chrono::seconds(15);
    while (std::chrono::steady_clock::now() < end) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        require(bool(client.GetReplicaList(key)), "query during expiry");
        auto next = client.PromotionObjectHeartbeatV2(holder);
        require(bool(next), "poll during expiry");
        if (!next->empty()) {
            fresh_id = next->front().task_id;
            fresh_found = fresh_id != old_id;
            break;
        }
    }
    require(fresh_found, "old task must expire and fresh UUID be admitted");
    require(bool(client.PromotionAllocStartV2(holder, key, "default", 1024,
                                              {segment.name}, fresh_id)),
            "allocate fresh");
    require(!client.PromotionAllocStartV2(holder, key, "default", 1024,
                                          {segment.name}, old_id),
            "stale alloc accepted");
    require(!client.NotifyPromotionSuccessV2(holder, key, "default", old_id),
            "stale success accepted");
    require(!client.NotifyPromotionFailureV2(holder, key, "default", old_id),
            "stale failure accepted");
    require(
        bool(client.NotifyPromotionSuccessV2(holder, key, "default", fresh_id)),
        "fresh task damaged by stale callbacks");
    auto result = client.GetReplicaList(key);
    require(
        result && std::any_of(result->replicas.begin(), result->replicas.end(),
                              [](const auto& r) {
                                  return r.is_memory_replica() &&
                                         r.status == ReplicaStatus::COMPLETE;
                              }),
        "fresh replica must remain COMPLETE");
    require(bool(client.Remove(key, true)), "cleanup");
    std::cout << "PASS actual RPC expiry/re-admission/stale UUID "
                 "rejection/fresh commit; synthetic control-plane only\n";
}
