#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace mooncake::shared_cache_diagnostics {

enum class Kind { SelectedRead, ReadReturn, BackendRead, PromotionCommit,
                  PromotionFailure };
enum class Tier { Unknown, Memory, NoF, LocalDisk, Disk, Dfs };
enum class Backend { None, FilePerKey, Bucket, OffsetAllocator };

struct Config {
    std::string case_id, epoch, tenant_id, key_salt, rank, component;
    std::vector<std::string> key_ids;
    uint64_t max_keys = 64, max_events = 1024, max_bytes = 1048576;
    uint64_t max_duration_ms = 30000, max_requests = 1, queue_capacity = 256;
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
    bool BeginReadKeys(const std::string& tenant, const std::vector<std::string>& keys) noexcept;
    void Lost() noexcept;
    void Emit(const std::string& tenant, const std::string& key,
              const Event&) noexcept;
    Snapshot Drain() noexcept;
    static Capture& Global() noexcept;

 private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Fail-closed plan gate used by a future authenticated original-writer control
// integration. All facts MUST be read under the same object metadata lock and
// backed by an SSD retention guard held through the subsequent read handoff.
// This does not authorize calling the legacy, racy BatchReplicaClear API.
struct ClearFacts {
    bool enabled = false, manifest_matches = false, original_writer = false;
    bool lease_expired = false, target_is_memory = false;
    bool all_replicas_complete = false, ssd_exact_complete = false;
    bool owner_readable = false, no_inflight_operations = false;
    bool ssd_retention_held = false;
    std::string segment;
};
bool CanClearMemoryUnderLock(const ClearFacts&) noexcept;

}  // namespace mooncake::shared_cache_diagnostics
