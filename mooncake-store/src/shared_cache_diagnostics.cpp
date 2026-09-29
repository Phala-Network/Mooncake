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

Capture::Capture() = default;
Capture::Capture(Config c) : impl_(std::make_unique<Impl>(std::move(c))) {}
Capture::~Capture() = default;
bool Capture::Enabled() const noexcept { return impl_ && !impl_->expired.load(); }
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
    } catch (...) { result.complete = false; }
    return result;
}

Capture& Capture::Global() noexcept {
    struct Runtime {
        Capture disabled;
        std::unique_ptr<Capture> capture;
        std::thread worker;
        std::atomic<bool> stop{false};
        int fd = -1;
        Runtime() noexcept {
            const char* path = std::getenv("MOONCAKE_SHARED_CACHE_DIAGNOSTICS_MANIFEST");
            if (!path || !*path) return;
            int input = -1;
            try {
                input = open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
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
            } catch (...) {
                if (input>=0) close(input);
                if (fd>=0) { close(fd); fd=-1; }
                capture.reset();
                // Fixed message only; parser errors can contain salt/input.
                const char message[]="Mooncake shared cache diagnostics disabled: invalid manifest/output\n";
                write(STDERR_FILENO,message,sizeof(message)-1);
            }
        }
        ~Runtime() { stop=true; if(worker.joinable())worker.join(); if(fd>=0)close(fd); }
    };
    static Runtime runtime;
    return runtime.capture ? *runtime.capture : runtime.disabled;
}

bool CanClearMemoryUnderLock(const ClearFacts& f) noexcept {
    return f.enabled && f.manifest_matches && f.original_writer &&
           f.lease_expired && f.target_is_memory && !f.segment.empty() &&
           f.all_replicas_complete && f.ssd_exact_complete && f.owner_readable &&
           f.no_inflight_operations && f.ssd_retention_held;
}
}  // namespace mooncake::shared_cache_diagnostics
