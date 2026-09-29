#include "shared_cache_diagnostics.h"

#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <json/json.h>

#include <atomic>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <deque>
#include <fcntl.h>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <unordered_set>
#include <unordered_map>
#include <sys/stat.h>
#include <unistd.h>

namespace mooncake::shared_cache_diagnostics {
namespace {
using Clock = std::chrono::steady_clock;
constexpr uint64_t kSummaryReserve = 1024;
thread_local ReadPurpose read_purpose=ReadPurpose::Unknown;
bool Label(const std::string& s) {
    if (s.empty() || s.size() > 96) return false;
    for (unsigned char c : s)
        if (!(std::isalnum(c) || c == '_' || c == '-' || c == '.')) return false;
    return true;
}
bool Hex(const std::string& s) {
    if (s.size() != 64) return false;
    for (char c : s) if (!(c >= '0' && c <= '9') && !(c >= 'a' && c <= 'f')) return false;
    return true;
}
std::string EncodeJson(const Json::Value& v) {
    Json::StreamWriterBuilder builder;
    builder["indentation"] = "";
    return Json::writeString(builder, v) + "\n";
}
const char* Name(Kind k) {
    switch (k) {
        case Kind::SelectedRead: return "selected_read";
        case Kind::ReadReturn: return "read_return";
        case Kind::BackendRead: return "backend_read_return";
        case Kind::PromotionCommit: return "promotion_commit";
        case Kind::PromotionFailure: return "promotion_failure";
    }
    return "unknown";
}
const char* Name(Tier t) {
    switch (t) {
        case Tier::Memory: return "MEMORY";
        case Tier::NoF: return "NOF";
        case Tier::LocalDisk: return "LOCAL_DISK";
        case Tier::Disk: return "DISK";
        case Tier::Dfs: return "DFS";
        default: return "UNKNOWN";
    }
}
const char* Name(Backend b) {
    switch (b) {
        case Backend::FilePerKey: return "file_per_key";
        case Backend::Bucket: return "bucket";
        case Backend::OffsetAllocator: return "offset_allocator";
        default: return "none";
    }
}
bool WriteAll(int fd, const std::string& s) {
    size_t done = 0;
    while (done < s.size()) {
        auto n = write(fd, s.data() + done, s.size() - done);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return false;
        done += static_cast<size_t>(n);
    }
    return true;
}
}  // namespace

std::string Identifier(const Config& c, const std::string& domain,
                       const std::string& tenant, const std::string& value) {
    std::string data = domain;
    data.push_back('\0');
    for (const auto* field : {&c.case_id, &c.epoch, &tenant, &value}) {
        if (field->size() > 65536) throw std::invalid_argument("diagnostic field too large");
        const auto n = static_cast<uint32_t>(field->size());
        for (int shift : {24, 16, 8, 0}) data.push_back(static_cast<char>(n >> shift));
        data += *field;
    }
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int length = 0;
    if (!HMAC(EVP_sha256(), c.key_salt.data(), static_cast<int>(c.key_salt.size()),
              reinterpret_cast<const unsigned char*>(data.data()), data.size(),
              digest, &length) || length != 32)
        throw std::runtime_error("diagnostic HMAC failed");
    static constexpr char hex[] = "0123456789abcdef";
    std::string result;
    result.reserve(64);
    for (unsigned int i = 0; i < length; ++i) {
        result += hex[digest[i] >> 4]; result += hex[digest[i] & 15];
    }
    return result;
}

struct Capture::Impl {
    Config config;
    std::unordered_set<std::string> allowed;
    std::unordered_map<std::string,uint64_t> reads;
    std::mutex mutex;
    std::deque<std::string> queue;
    Clock::time_point deadline;
    uint64_t emitted = 0, dropped = 0, rejected = 0, bytes = 0, attempts = 0;
    std::atomic<bool> expired{false};
    explicit Impl(Config c) : config(std::move(c)) {
        if (!Label(config.case_id) || !Label(config.epoch) || !Label(config.rank) ||
            !Label(config.component) || config.tenant_id.empty() ||
            config.tenant_id.size() > 256 || config.key_salt.size() < 16 ||
            config.key_salt.size() > 256 || !config.max_keys || config.max_keys > 256 ||
            config.key_ids.empty() || config.key_ids.size() > config.max_keys ||
            !config.max_events || config.max_events > 8192 ||
            config.max_bytes < 4096 || config.max_bytes > 16 * 1024 * 1024 ||
            !config.max_duration_ms || config.max_duration_ms > 120000 ||
            config.max_requests != 1 || !config.queue_capacity || config.queue_capacity > 1024)
            throw std::invalid_argument("invalid bounded diagnostic manifest");
        for (const auto& id : config.key_ids)
            if (!Hex(id) || !allowed.insert(id).second)
                throw std::invalid_argument("invalid diagnostic key allowlist");
        deadline = Clock::now() + std::chrono::milliseconds(config.max_duration_ms);
    }
};

ReadPurposeScope::ReadPurposeScope(ReadPurpose value) noexcept {
    if (Capture::Global().Enabled()) {
        active_=true; previous_=read_purpose; read_purpose=value;
    }
}
ReadPurposeScope::~ReadPurposeScope() { if(active_) read_purpose=previous_; }
ReadPurpose ReadPurposeScope::Current() noexcept { return read_purpose; }

Capture::Capture() = default;
Capture::Capture(Config c) : impl_(std::make_unique<Impl>(std::move(c))) {}
Capture::~Capture() = default;
bool Capture::Enabled() const noexcept { return impl_ && !impl_->expired.load(); }
bool Capture::OwnerEnabled() const noexcept {
    return Enabled() && impl_->config.capture_owner_drain;
}
void Capture::EmitOwner(const OwnerDrainSnapshot& owner) noexcept {
    if (!OwnerEnabled()) return;
    try {
        std::lock_guard<std::mutex> guard(impl_->mutex);
        auto& s = *impl_;
        if (Clock::now() >= s.deadline) {
            s.expired = true;
            ++s.dropped;
            return;
        }
        if (++s.attempts > s.config.max_events * 4 ||
            s.emitted >= s.config.max_events ||
            s.queue.size() >= s.config.queue_capacity) {
            ++s.dropped;
            return;
        }
        Json::Value v;
        v["schema"] = "phala.shared-cache.owner-drain.v1";
        v["kind"] = "owner_drain";
        v["case_id"] = s.config.case_id;
        v["epoch"] = s.config.epoch;
        v["tenant_id"] = s.config.tenant_id;
        v["pid"] = Json::Int64(getpid());
        const auto id = [&](const char* domain, const std::string& raw) {
            return Identifier(s.config, domain, s.config.tenant_id, raw);
        };
        v["owner_id"] = id("phala.shared-cache-owner.v1", owner.owner);
        v["store_instance_id"] =
            id("phala.shared-cache-owner.v1", owner.instance);
        v["scope_id"] = id("phala.shared-cache-owner.v1", owner.scope);
        v["backend_path_id"] = id("phala.shared-cache-file.v1", owner.path);
        v["owner_client_requested_tenant_id"] = owner.client_tenant;
        v["coverage"] = "owner_global_bucket";
        v["sample_time_unix_ms"] = Json::UInt64(owner.sample_time_unix_ms);
        v["init_completed_unix_ms"] =
            Json::UInt64(owner.init_completed_unix_ms);
        v["scan_completed_unix_ms"] =
            Json::UInt64(owner.scan_completed_unix_ms);
        v["owner_sample_sequence"] = Json::UInt64(owner.sample_sequence);
        v["activity_sequence"] = Json::UInt64(owner.activity_sequence);
        const char* names[] = {
            "loads",      "offloads", "promotions",         "removes",
            "heartbeats", "rescans",  "leased_read_buffers"};
        for (size_t i = 0; i < owner.active.size(); ++i)
            v["active"][names[i]] = Json::UInt64(owner.active[i]);
        v["pending"]["backend_writes"] = Json::UInt64(owner.pending_writes);
        v["pending"]["backend_evictions"] =
            Json::UInt64(owner.pending_evictions);
        v["pending"]["ungrouped_offloads"] =
            Json::UInt64(owner.pending_ungrouped);
        v["pending"]["read_guards"] = Json::UInt64(owner.read_guards);
        v["bucket_count"] = Json::UInt64(owner.bucket_count);
        v["covered_buckets"] = Json::UInt64(owner.covered_buckets);
        v["available"] = owner.available;
        v["consistent"] = owner.consistent;
        v["backend_initialized"] = owner.backend_initialized;
        v["bucket_eviction_policy"] = owner.eviction_policy;
        v["disk_watermark_eviction"] = owner.watermark_eviction;
        v["metadata_resync_pending"] = owner.resync_pending;
        v["draining"] = owner.draining;
        v["heartbeat_ok"] = owner.heartbeat_ok;
        const auto line = EncodeJson(v);
        if (line.size() > 4096 ||
            s.bytes + line.size() + kSummaryReserve > s.config.max_bytes) {
            ++s.dropped;
            return;
        }
        s.queue.push_back(line);
        s.bytes += line.size();
        ++s.emitted;
    } catch (...) {
        Lost();
    }
}
bool Capture::BeginReadKeys(const std::string& tenant, const std::vector<std::string>& keys) noexcept {
    if (!Enabled()) return false;
    try {
        std::lock_guard<std::mutex> guard(impl_->mutex);
        auto& s=*impl_;
        if (Clock::now()>=s.deadline) { s.expired=true; ++s.dropped; return false; }
        if (keys.size()>256) { ++s.dropped; return false; }
        if (tenant!=s.config.tenant_id) return false;
        bool matched=false;
        for (const auto& key:keys) {
            if (++s.attempts>s.config.max_events*4) { ++s.dropped; return false; }
            const auto id=Identifier(s.config,"phala.shared-cache-key.v1",tenant,key);
            if (!s.allowed.count(id)) continue;
            matched=true;
            // Several component batches belong to one engine request. Only
            // repeated reads of the same allowlisted object are ambiguous.
            if (++s.reads[id]>s.config.max_requests) ++s.dropped;
        }
        return matched;
    } catch (...) { Lost(); return false; }
}
void Capture::Lost() noexcept {
    if (!impl_) return;
    try { std::lock_guard<std::mutex> g(impl_->mutex); ++impl_->dropped; } catch (...) {}
}
void Capture::Emit(const std::string& tenant, const std::string& key,
                   const Event& event) noexcept {
    if (!Enabled()) return;
    try {
        std::lock_guard<std::mutex> guard(impl_->mutex);
        auto& s = *impl_;
        if (Clock::now() >= s.deadline) { s.expired = true; ++s.dropped; return; }
        if (tenant != s.config.tenant_id) { ++s.rejected; return; }
        if (++s.attempts>s.config.max_events*4) { ++s.dropped; return; }
        const auto id = Identifier(s.config, "phala.shared-cache-key.v1", tenant, key);
        if (!s.allowed.count(id)) { ++s.rejected; return; }
        if (s.emitted >= s.config.max_events || s.queue.size() >= s.config.queue_capacity) {
            ++s.dropped; return;
        }
        Json::Value v;
        v["schema"] = "phala.shared-cache.native.v1";
        v["case_id"] = s.config.case_id; v["epoch"] = s.config.epoch;
        v["rank"] = s.config.rank; v["component"] = s.config.component;
        v["pid"] = Json::Int64(getpid());
        v["key_id"] = id; v["kind"] = Name(event.kind); v["tier"] = Name(event.tier);
        v["read_purpose"] = event.purpose==ReadPurpose::ConsumerGet ? "consumer_get" : event.purpose==ReadPurpose::Promotion ? "promotion" : "unknown";
        v["backend"] = Name(event.backend); v["sequence"] = Json::UInt64(s.emitted + 1);
        v["replica_id"] = Json::UInt64(event.replica);
        v["requested_bytes"] = Json::UInt64(event.requested_bytes);
        v["returned_bytes"] = Json::Int64(event.returned_bytes);
        v["error"] = event.error; v["committed"] = event.committed;
        v["offset"] = Json::UInt64(event.offset);
        if (!event.owner.empty()) v["owner_id"] = Identifier(s.config, "phala.shared-cache-owner.v1", tenant, event.owner);
        if (!event.physical_file.empty()) v["file_id"] = Identifier(s.config, "phala.shared-cache-file.v1", tenant, event.physical_file);
        const auto line = EncodeJson(v);
        if (line.size() > 2048 || s.bytes + line.size() + kSummaryReserve > s.config.max_bytes) {
            ++s.dropped; return;
        }
        s.queue.push_back(line); s.bytes += line.size(); ++s.emitted;
    } catch (...) {
        // Diagnostics are never allowed to throw into a serving path.
        if (impl_) { try { std::lock_guard<std::mutex> g(impl_->mutex); ++impl_->dropped; } catch (...) {} }
    }
}
Snapshot Capture::Drain() noexcept {
    Snapshot result;
    if (!impl_) return result;
    try {
        std::lock_guard<std::mutex> guard(impl_->mutex);
        result.events.assign(impl_->queue.begin(), impl_->queue.end());
        impl_->queue.clear(); result.emitted = impl_->emitted;
        result.dropped = impl_->dropped; result.rejected = impl_->rejected;
        result.bytes = impl_->bytes; result.complete = !result.dropped;
    } catch (...) { Lost(); result.complete = false; }
    return result;
}

Capture& Capture::Global() noexcept {
    struct Runtime {
        Capture disabled;
        std::unique_ptr<Capture> capture;
        std::thread worker;
        std::atomic<bool> stop{false};
        int fd = -1;
        std::atomic<Capture*> active{nullptr};
        std::atomic<bool> waiting{false};
        std::mutex arm_mutex;
        std::string manifest_path;
        Runtime() noexcept {
            try {
                const char* path =
                    std::getenv("MOONCAKE_SHARED_CACHE_DIAGNOSTICS_MANIFEST");
                if (!path || !*path) return;
                manifest_path = path;
                Load();
            } catch (...) {
            }
        }
        void Load() noexcept {
            const char* path = manifest_path.c_str();
            int input = -1;
            try {
                input =
                    open(path, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
                if (input < 0 && errno == ENOENT) {
                    waiting = true;
                    return;
                }
                waiting = false;
                struct stat st{};
                if (input < 0 || fstat(input, &st) || !S_ISREG(st.st_mode) ||
                    st.st_uid != geteuid() || (st.st_mode & 077) || st.st_size <= 0 || st.st_size > 65536)
                    throw std::runtime_error("diagnostic manifest rejected");
                std::string data(static_cast<size_t>(st.st_size), '\0');
                size_t pos = 0;
                while (pos < data.size()) {
                    auto n = read(input, data.data() + pos, data.size() - pos);
                    if (n < 0 && errno == EINTR) continue;
                    if (n <= 0) throw std::runtime_error("diagnostic manifest short read");
                    pos += static_cast<size_t>(n);
                }
                close(input); input = -1;
                Json::Value v; Json::CharReaderBuilder reader; std::string errors;
                reader["rejectDupKeys"] = true;
                auto parser = std::unique_ptr<Json::CharReader>(reader.newCharReader());
                if (!parser->parse(data.data(), data.data() + data.size(), &v, &errors))
                    throw std::runtime_error("invalid diagnostic manifest JSON");
                Config c;
                if (v.isMember("capture_owner_drain") &&
                    !v["capture_owner_drain"].isBool())
                    throw std::runtime_error("invalid diagnostic owner mode");
                c.capture_owner_drain =
                    v.get("capture_owner_drain", false).asBool();
                c.case_id=v["case_id"].asString(); c.epoch=v["epoch"].asString();
                c.tenant_id=v["tenant_id"].asString(); c.key_salt=v["key_salt"].asString();
                c.rank=v["rank"].asString(); c.component=v["component"].asString();
                for (const auto& key : v["key_ids"]) c.key_ids.push_back(key.asString());
                c.max_keys=v["max_keys"].asUInt64(); c.max_events=v["max_events"].asUInt64();
                c.max_bytes=v["max_bytes"].asUInt64(); c.max_duration_ms=v["max_duration_ms"].asUInt64();
                c.max_requests=v["max_requests"].asUInt64();
                capture=std::make_unique<Capture>(c);
                fd=open(v["output_path"].asCString(), O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0600);
                if (fd < 0) throw std::runtime_error("diagnostic output rejected");
                worker=std::thread([this,c] {
                  try {
                    const auto deadline=Clock::now()+std::chrono::milliseconds(c.max_duration_ms);
                    bool ok=true;
                    while (!stop && Clock::now()<deadline) {
                        auto s=capture->Drain();
                        for (const auto& line:s.events) ok=WriteAll(fd,line)&&ok;
                        std::this_thread::sleep_for(std::chrono::milliseconds(10));
                    }
                    capture->impl_->expired=true;
                    auto s=capture->Drain();
                    for (const auto& line:s.events) ok=WriteAll(fd,line)&&ok;
                    Json::Value summary;
                    summary["kind"]="capture_summary"; summary["emitted"]=Json::UInt64(s.emitted);
                    summary["dropped"]=Json::UInt64(s.dropped); summary["rejected"]=Json::UInt64(s.rejected);
                    summary["complete"]=s.complete&&ok; summary["bytes"]=Json::UInt64(s.bytes);
                    WriteAll(fd,EncodeJson(summary));
                  } catch (...) {
                    capture->impl_->expired=true;
                    WriteAll(fd,"{\"kind\":\"capture_summary\",\"complete\":false,\"writer_failed\":true}\n");
                  }
                });
                active.store(capture.get(), std::memory_order_release);
            } catch (...) {
                if (input>=0) close(input);
                if (fd>=0) { close(fd); fd=-1; }
                capture.reset();
                waiting = false;
                // Fixed message only; parser errors can contain salt/input.
                const char message[]="Mooncake shared cache diagnostics disabled: invalid manifest/output\n";
                const auto written=write(STDERR_FILENO,message,sizeof(message)-1);
                (void)written;
            }
        }
        Capture& Get() noexcept {
            if (auto* current = active.load(std::memory_order_acquire))
                return *current;
            if (!waiting.load()) return disabled;
            try {
                std::lock_guard<std::mutex> guard(arm_mutex);
                if (auto* current = active.load(std::memory_order_acquire))
                    return *current;
                if (!waiting.load()) return disabled;
                Load();
                if (auto* current = active.load(std::memory_order_acquire))
                    return *current;
            } catch (...) {
                waiting = false;
            }
            return disabled;
        }
        ~Runtime() { stop=true; if(worker.joinable())worker.join(); if(fd>=0)close(fd); }
    };
    static Runtime runtime;
    return runtime.Get();
}

std::optional<SnapshotRequest> AuthorizeSnapshot(
    const std::string& tenant, const std::string& case_id,
    const std::string& epoch, const std::string& sample_id,
    const std::string& key_ids) noexcept {
    struct Policy {
        std::mutex mutex;
        std::optional<SnapshotRequest> request;
        std::unordered_set<std::string> samples;
        uint64_t remaining = 0;
        Policy() noexcept {
            int fd = -1;
            try {
                const char* path =
                    std::getenv("MOONCAKE_SHARED_CACHE_SNAPSHOT_MANIFEST");
                if (!path || !*path) return;
                fd = open(path, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
                struct stat st{};
                if (fd < 0 || fstat(fd, &st) || !S_ISREG(st.st_mode) ||
                    st.st_uid != geteuid() || (st.st_mode & 0777) != 0600 ||
                    st.st_size <= 0 || st.st_size > 65536)
                    throw std::invalid_argument("manifest");
                std::string data(static_cast<size_t>(st.st_size), '\0');
                size_t pos = 0;
                while (pos < data.size()) {
                    const auto n =
                        read(fd, data.data() + pos, data.size() - pos);
                    if (n < 0 && errno == EINTR) continue;
                    if (n <= 0) throw std::invalid_argument("manifest");
                    pos += static_cast<size_t>(n);
                }
                close(fd);
                fd = -1;
                Json::CharReaderBuilder reader;
                reader["rejectDupKeys"] = true;
                reader["failIfExtra"] = true;
                Json::Value v;
                std::string errors;
                std::unique_ptr<Json::CharReader> parser(
                    reader.newCharReader());
                if (!parser->parse(data.data(), data.data() + data.size(), &v,
                                   &errors) ||
                    !v.isObject())
                    throw std::invalid_argument("manifest");
                for (const char* name :
                     {"tenant_id", "case_id", "epoch", "key_salt"})
                    if (!v[name].isString())
                        throw std::invalid_argument("manifest");
                for (const char* name :
                     {"max_snapshots", "max_duration_ms", "max_response_bytes",
                      "max_total_logical_bytes"})
                    if (!v[name].isUInt64())
                        throw std::invalid_argument("manifest");
                SnapshotRequest q;
                q.identity.tenant_id = v["tenant_id"].asString();
                q.identity.case_id = v["case_id"].asString();
                q.identity.epoch = v["epoch"].asString();
                q.identity.key_salt = v["key_salt"].asString();
                remaining = v["max_snapshots"].asUInt64();
                const auto duration = v["max_duration_ms"].asUInt64();
                q.max_response_bytes = v["max_response_bytes"].asUInt64();
                q.max_total_logical_bytes =
                    v["max_total_logical_bytes"].asUInt64();
                if (!Label(q.identity.case_id) || !Label(q.identity.epoch) ||
                    q.identity.tenant_id.empty() ||
                    q.identity.tenant_id.size() > 256 ||
                    q.identity.key_salt.size() < 16 ||
                    q.identity.key_salt.size() > 256 || !remaining ||
                    remaining > 16 || !duration || duration > 120000 ||
                    q.max_response_bytes < 4096 ||
                    q.max_response_bytes > 1048576 ||
                    !q.max_total_logical_bytes ||
                    q.max_total_logical_bytes > 17179869184ULL ||
                    !v["keys"].isArray() || v["keys"].empty() ||
                    v["keys"].size() > 256)
                    throw std::invalid_argument("manifest");
                std::unordered_set<std::string> ids;
                for (const auto& item : v["keys"]) {
                    if (!item["key"].isString() || !item["key_id"].isString())
                        throw std::invalid_argument("manifest");
                    auto key = item["key"].asString();
                    auto id = item["key_id"].asString();
                    if (key.empty() || key.size() > 4096 ||
                        key.find_first_of(",*?[]") != std::string::npos ||
                        !Hex(id) || !ids.insert(id).second)
                        throw std::invalid_argument("manifest");
                    for (unsigned char c : key)
                        if (c < 32 || c == 127)
                            throw std::invalid_argument("manifest");
                    if (Identifier(q.identity, "phala.shared-cache-key.v1",
                                   q.identity.tenant_id, key) != id)
                        throw std::invalid_argument("manifest");
                    q.keys.emplace_back(std::move(id), std::move(key));
                }
                q.deadline = Clock::now() + std::chrono::milliseconds(duration);
                request = std::move(q);
            } catch (...) {
                if (fd >= 0) close(fd);
                request.reset();
                remaining = 0;
            }
        }
    };
    static Policy policy;
    try {
        std::lock_guard<std::mutex> guard(policy.mutex);
        if (!policy.request || !policy.remaining ||
            Clock::now() >= policy.request->deadline || !Label(sample_id) ||
            key_ids.size() > 16640 ||
            tenant != policy.request->identity.tenant_id ||
            case_id != policy.request->identity.case_id ||
            epoch != policy.request->identity.epoch ||
            policy.samples.count(sample_id))
            return std::nullopt;
        std::unordered_set<std::string> requested;
        size_t start = 0;
        while (start <= key_ids.size()) {
            const auto end = key_ids.find(',', start);
            const auto id = key_ids.substr(
                start, end == std::string::npos ? end : end - start);
            if (!Hex(id) || !requested.insert(id).second) return std::nullopt;
            if (end == std::string::npos) break;
            start = end + 1;
        }
        if (requested.size() != policy.request->keys.size())
            return std::nullopt;
        for (const auto& key : policy.request->keys)
            if (!requested.count(key.first)) return std::nullopt;
        auto result = *policy.request;
        result.sample_id = sample_id;
        policy.samples.insert(sample_id);
        --policy.remaining;
        return result;
    } catch (...) {
        return std::nullopt;
    }
}

}  // namespace mooncake::shared_cache_diagnostics
