#include "shared_cache_diagnostics.h"

#include <cassert>
#include <atomic>
#include <chrono>
#include <iostream>
#include <thread>
#include <filesystem>
#include <cstdlib>

using namespace mooncake::shared_cache_diagnostics;

Config Fixture() {
    Config c;
    c.case_id="case1"; c.epoch="epoch1"; c.tenant_id="default";
    c.key_salt="fixture-salt-0123456789"; c.rank="0"; c.component="fixture";
    c.key_ids={Identifier(c,"phala.shared-cache-key.v1","default","component-key")};
    return c;
}

int main(int argc, char** argv) {
    if (argc > 1 && std::string(argv[1]) == "--snapshot-policy") {
        auto q = AuthorizeSnapshot("default", "case1", "epoch1", "sample1",
                                   Fixture().key_ids[0]);
        if (!q) return 3;
        assert(!AuthorizeSnapshot("default", "case1", "epoch1", "sample1",
                                  Fixture().key_ids[0]));
        return 0;
    }
    if (argc > 2 && std::string(argv[1]).rfind("--late", 0) == 0) {
        assert(!Capture::Global().Enabled());
        if (std::string(argv[1]) == "--late-delayed")
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        const auto manifest =
            std::getenv("MOONCAKE_SHARED_CACHE_DIAGNOSTICS_MANIFEST");
        std::filesystem::rename(argv[2], manifest);
        if (std::string(argv[1]) == "--late-invalid") {
            assert(!Capture::Global().Enabled());
            std::filesystem::rename(argv[3], manifest);
            return Capture::Global().Enabled() ? 9 : 3;
        }
        if (!Capture::Global().Enabled()) return 3;
        std::vector<std::thread> threads;
        for (int i = 0; i < 8; ++i)
            threads.emplace_back([] {
                auto& capture = Capture::Global();
                assert(capture.Enabled());
                Event event{Kind::BackendRead};
                capture.Emit("default", "component-key", event);
            });
        for (auto& thread : threads) thread.join();
        return 0;
    }
    if (argc>1) {
        auto& runtime=Capture::Global();
        if (!runtime.Enabled()) return 3;
        std::atomic<int> ready{0};
        auto emit=[&](ReadPurpose purpose,int bytes) {
            assert(ReadPurposeScope::Current()==ReadPurpose::Unknown);
            {
                ReadPurposeScope scope(purpose);
                ++ready;
                while(ready.load()<2) std::this_thread::yield();
                assert(ReadPurposeScope::Current()==purpose);
                Event event{Kind::BackendRead}; event.returned_bytes=bytes;
                event.purpose=ReadPurposeScope::Current();
                runtime.Emit("default","component-key",event);
                { ReadPurposeScope nested(ReadPurpose::Unknown); }
                assert(ReadPurposeScope::Current()==purpose);
            }
            assert(ReadPurposeScope::Current()==ReadPurpose::Unknown);
        };
        std::thread consumer([&]{emit(ReadPurpose::ConsumerGet,123);});
        std::thread promotion([&]{emit(ReadPurpose::Promotion,456);});
        consumer.join(); promotion.join();
        assert(ReadPurposeScope::Current()==ReadPurpose::Unknown);
        return 0;
    }
    auto config=Fixture();
    assert(config.key_ids[0]=="6ef3b9cff0cca0e84d254e750accc0d9ddf413eb8fda38d47c58e461106ac6b3");
    assert(Identifier(config,"phala.shared-cache-request.v1","default","component-key")!=config.key_ids[0]);
    assert(Identifier(config,"phala.shared-cache-key.v1","other","component-key")!=config.key_ids[0]);
    assert(Identifier(config,"phala.shared-cache-key.v1","default","component-key-more")!=config.key_ids[0]);

    Capture disabled;
    assert(!disabled.Enabled() && !disabled.BeginReadKeys("default",{"component-key"}));
    Event event{Kind::BackendRead}; event.tier=Tier::LocalDisk;
    event.backend=Backend::OffsetAllocator; event.requested_bytes=4096;
    event.returned_bytes=4096; event.owner="private-owner-address";
    event.physical_file="private-file-name";
    disabled.Emit("default",std::string(70000,'x'),event);
    assert(disabled.Drain().events.empty());

    auto multipool=config;
    multipool.key_ids.push_back(Identifier(config,"phala.shared-cache-key.v1","default","second-pool"));
    Capture pools(multipool);
    assert(!pools.BeginReadKeys("default",{"unrelated"}));
    assert(pools.BeginReadKeys("default",{"component-key"}));
    assert(pools.BeginReadKeys("default",{"second-pool"}));
    assert(pools.Drain().complete);
    assert(pools.BeginReadKeys("default",{"component-key"}));
    assert(!pools.Drain().complete);
    Capture capture(config);
    assert(capture.BeginReadKeys("default",{"component-key"}));
    capture.Emit("other","component-key",event);
    capture.Emit("default","not-allowed",event);
    capture.Emit("default","component-key",event);
    auto snapshot=capture.Drain();
    assert(snapshot.events.size()==1 && snapshot.rejected==2 && snapshot.complete);
    const auto& line=snapshot.events[0];
    for (const auto& secret:{"component-key","fixture-salt","private-owner-address","private-file-name"})
        assert(line.find(secret)==std::string::npos);
    assert(line.find(config.key_ids[0])!=std::string::npos);
    assert(line.find("\"returned_bytes\":4096")!=std::string::npos);

    event.returned_bytes=3; event.error=1;
    capture.Emit("default","component-key",event);
    snapshot=capture.Drain();
    assert(snapshot.events[0].find("\"returned_bytes\":3")!=std::string::npos);
    assert(snapshot.events[0].find("\"error\":1")!=std::string::npos);
    assert(capture.BeginReadKeys("default",{"component-key"}));
    assert(!capture.Drain().complete);  // duplicate/concurrent request is incomplete

    auto bounded=config; bounded.max_events=1;
    Capture limit(bounded);
    limit.Emit("default","component-key",event);
    limit.Emit("default","component-key",event);
    snapshot=limit.Drain(); assert(snapshot.emitted==1 && snapshot.dropped==1 && !snapshot.complete);

    bounded=config; bounded.queue_capacity=1;
    Capture queue(bounded);
    queue.Emit("default","component-key",event); queue.Emit("default","component-key",event);
    assert(queue.Drain().dropped==1);

    bounded=config; bounded.max_bytes=4096;
    Capture bytes(bounded);
    for(int i=0;i<100;++i) bytes.Emit("default","component-key",event);
    snapshot=bytes.Drain(); assert(snapshot.bytes+1024<=4096 && snapshot.dropped>0);

    bounded=config; bounded.max_duration_ms=1;
    Capture deadline(bounded);
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    deadline.Emit("default","component-key",event);
    assert(deadline.Drain().dropped==1 && !deadline.Enabled());

    Capture parallel(config);
    std::thread first([&]{for(int i=0;i<30;++i)parallel.Emit("default","component-key",event);});
    std::thread second([&]{for(int i=0;i<30;++i)parallel.Emit("default","component-key",event);});
    first.join(); second.join(); assert(parallel.Drain().emitted==60);

    std::cout << "shared cache diagnostic CPU fixtures passed\n";
}
