#pragma once

#include <cstdint>
#include <array>
#include <chrono>
#include <optional>
#include <utility>
#include <memory>
#include <string>
#include <vector>

namespace mooncake::shared_cache_diagnostics {

enum class Kind { SelectedRead, ReadReturn, BackendRead, PromotionCommit,
                  PromotionFailure };
enum class Tier { Unknown, Memory, NoF, LocalDisk, Disk, Dfs };
enum class Backend { None, FilePerKey, Bucket, OffsetAllocator };
enum class ReadPurpose { Unknown, ConsumerGet, Promotion };

// The backend calls are synchronous. Distinguish the consumer read from the
// subsequent promotion copy without inventing an RPC operation identifier.
class ReadPurposeScope {
 public:
    explicit ReadPurposeScope(ReadPurpose) noexcept;
    ~ReadPurposeScope();
    ReadPurposeScope(const ReadPurposeScope&) = delete;
    ReadPurposeScope& operator=(const ReadPurposeScope&) = delete;
    static ReadPurpose Current() noexcept;
 private:
    ReadPurpose previous_ = ReadPurpose::Unknown;
    bool active_ = false;
};

struct Config {
    bool capture_owner_drain = false;
    std::string case_id, epoch, tenant_id, key_salt, rank, component;
    std::vector<std::string> key_ids;
    uint64_t max_keys = 64, max_events = 1024, max_bytes = 1048576;
    uint64_t max_duration_ms = 30000, max_requests = 1, queue_capacity = 256;
};

// Process-local owner observations, not a retention lock or storage generation.
struct OwnerDrainSnapshot {
    std::string owner, instance, scope, path, client_tenant;
    uint64_t sample_time_unix_ms = 0, init_completed_unix_ms = 0;
    uint64_t scan_completed_unix_ms = 0, sample_sequence = 0,
             activity_sequence = 0;
    // load, offload, promotion, remove/eviction, heartbeat, rescan, leased
    // buffer
    std::array<uint64_t, 7> active{};
    uint64_t pending_writes = 0, pending_evictions = 0, pending_ungrouped = 0;
    uint64_t read_guards = 0, bucket_count = 0, covered_buckets = 0;
    bool available = false, consistent = false, backend_initialized = false;
    bool watermark_eviction = true, resync_pending = false, draining = false;
    bool heartbeat_ok = false;
    std::string eviction_policy = "unknown";
};

// HMAC input is domain + NUL, then uint32 big-endian length-prefixed UTF-8
// case_id, epoch, tenant_id, actual key. The salt and raw key never leave here.
std::string Identifier(const Config&, const std::string& domain,
                       const std::string& tenant, const std::string& value);

struct Event {
    explicit Event(Kind value = Kind::SelectedRead) : kind(value) {}
    Kind kind = Kind::SelectedRead;
    Tier tier = Tier::Unknown;
    Backend backend = Backend::None;
    ReadPurpose purpose = ReadPurpose::Unknown;
    uint64_t replica = 0;
    uint64_t requested_bytes = 0;
    int64_t returned_bytes = -1;  // -1 means unknown, never invented success.
    int error = 0;
    std::string owner;           // HMAC before capture, not an emitted address.
    std::string physical_file;   // HMAC before capture, not an emitted path.
    uint64_t offset = 0;
    bool committed = false;
};

struct Snapshot {
    std::vector<std::string> events;
    uint64_t emitted = 0, dropped = 0, rejected = 0, bytes = 0;
    bool complete = true;
};

// Privileged finite admin snapshot. Raw keys/salt remain process-local.
struct SnapshotRequest {
    Config identity;
    std::string sample_id;
    std::vector<std::pair<std::string, std::string>> keys;  // id, raw key
    uint64_t max_response_bytes = 0, max_total_logical_bytes = 0;
    std::chrono::steady_clock::time_point deadline;
};
std::optional<SnapshotRequest> AuthorizeSnapshot(
    const std::string& tenant, const std::string& case_id,
    const std::string& epoch, const std::string& sample_id,
    const std::string& key_ids) noexcept;

// Bounded collector; no capture initialization or key hashing when disabled.
// Never changes serving results, including on allocation/serialization errors.
class Capture {
 public:
    Capture();
    explicit Capture(Config config);
    ~Capture();
    Capture(const Capture&) = delete;
    Capture& operator=(const Capture&) = delete;
    bool Enabled() const noexcept;
    bool OwnerEnabled() const noexcept;
    void EmitOwner(const OwnerDrainSnapshot&) noexcept;
    bool BeginReadKeys(const std::string& tenant, const std::vector<std::string>& keys) noexcept;
    void Lost() noexcept;
    void Emit(const std::string& tenant, const std::string& key,
              const Event&) noexcept;
    Snapshot Drain() noexcept;
    static Capture& Global() noexcept;
    static Capture& OwnerGlobal() noexcept;

   private:
    struct Runtime;
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace mooncake::shared_cache_diagnostics
