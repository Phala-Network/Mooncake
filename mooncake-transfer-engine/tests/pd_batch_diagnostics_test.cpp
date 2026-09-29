#include <gtest/gtest.h>

#include "multi_transport.h"

using namespace mooncake;

namespace {
class CompletingTransport : public Transport {
   public:
    explicit CompletingTransport(const char* name) : name_(name) {}
    TransferStatusEnum next = TransferStatusEnum::WAITING;
    size_t bytes = 0;
    size_t polls = 0;
    Status submitTransfer(BatchID, const std::vector<TransferRequest>&) override {
        return Status::OK();
    }
    Status getTransferStatus(BatchID id, size_t task_id,
                             TransferStatus& status) override {
        ++polls;
        status.s = next;
        status.transferred_bytes = bytes;
        if (next != TransferStatusEnum::WAITING)
            reinterpret_cast<BatchDesc*>(id)->task_list[task_id].is_finished = true;
        return Status::OK();
    }
   private:
    const char* getName() const override { return name_; }
    int registerLocalMemory(void*, size_t, const std::string&, bool, bool) override { return 0; }
    int unregisterLocalMemory(void*, bool) override { return 0; }
    int registerLocalMemoryBatch(const std::vector<BufferEntry>&, const std::string&) override { return 0; }
    int unregisterLocalMemoryBatch(const std::vector<void*>&) override { return 0; }
    const char* name_;
};

TEST(PDBatchDiagnostics, WaitsForEveryNativeTaskAndPreservesSelection) {
    std::string name = "offline";
    MultiTransport engine(nullptr, name);
    CompletingTransport nvlink("nvlink_intraNode");
    auto id = engine.allocateBatchID(2);
    auto& tasks = reinterpret_cast<Transport::BatchDesc*>(id)->task_list;
    tasks.emplace_back().transport_ = &nvlink;
    tasks.emplace_back().transport_ = &nvlink;
    Transport::TransferStatus status;
    ASSERT_TRUE(engine.getBatchTransferStatus(id, status).ok());
    EXPECT_EQ(status.s, Transport::TransferStatusEnum::WAITING);
    EXPECT_EQ(status.transferred_bytes, 0);
    EXPECT_FALSE(engine.freeBatchID(id).ok());
    Transport::BatchTransportSelection selection;
    ASSERT_TRUE(engine.getBatchTransportSelection(id, selection).ok());
    EXPECT_EQ(selection.task_count, 2);
    EXPECT_EQ(selection.selected_transports.at("nvlink_intraNode"), 2);
    EXPECT_EQ(selection.missing_transports, 0);
    nvlink.next = Transport::TransferStatusEnum::COMPLETED;
    nvlink.bytes = 123;
    ASSERT_TRUE(engine.getBatchTransferStatus(id, status).ok());
    EXPECT_EQ(status.s, Transport::TransferStatusEnum::COMPLETED);
    EXPECT_EQ(status.transferred_bytes, 246);
    EXPECT_EQ(nvlink.polls, 4);
    ASSERT_TRUE(engine.getBatchTransferStatus(id, status).ok());
    EXPECT_EQ(status.transferred_bytes, 246);
    EXPECT_EQ(nvlink.polls, 4);
    ASSERT_TRUE(engine.freeBatchID(id).ok());
}

TEST(PDBatchDiagnostics, ReportsMixedTransportAndNativeFailure) {
    std::string name = "offline";
    MultiTransport engine(nullptr, name);
    CompletingTransport nvlink("nvlink_intraNode"), tcp("tcp");
    auto id = engine.allocateBatchID(2);
    auto& tasks = reinterpret_cast<Transport::BatchDesc*>(id)->task_list;
    tasks.emplace_back().transport_ = &nvlink;
    tasks.emplace_back().transport_ = &tcp;
    Transport::BatchTransportSelection selection;
    ASSERT_TRUE(engine.getBatchTransportSelection(id, selection).ok());
    EXPECT_EQ(selection.selected_transports.size(), 2);
    EXPECT_EQ(selection.selected_transports.at("tcp"), 1);
    nvlink.next = Transport::TransferStatusEnum::COMPLETED;
    nvlink.bytes = 99;
    tcp.next = Transport::TransferStatusEnum::FAILED;
    Transport::TransferStatus status;
    ASSERT_TRUE(engine.getBatchTransferStatus(id, status).ok());
    EXPECT_EQ(status.s, Transport::TransferStatusEnum::FAILED);
    EXPECT_EQ(status.transferred_bytes, 99);
    ASSERT_TRUE(engine.freeBatchID(id).ok());
}

TEST(PDBatchDiagnostics, EmptyAndMissingTransportCannotProveNvlink) {
    std::string name = "offline";
    MultiTransport engine(nullptr, name);
    auto id = engine.allocateBatchID(1);
    Transport::BatchTransportSelection selection;
    ASSERT_TRUE(engine.getBatchTransportSelection(id, selection).ok());
    EXPECT_EQ(selection.task_count, 0);
    EXPECT_TRUE(selection.selected_transports.empty());
    auto& task = reinterpret_cast<Transport::BatchDesc*>(id)->task_list.emplace_back();
    task.is_finished = true;
    ASSERT_TRUE(engine.getBatchTransportSelection(id, selection).ok());
    EXPECT_EQ(selection.task_count, 1);
    EXPECT_EQ(selection.missing_transports, 1);
    ASSERT_TRUE(engine.freeBatchID(id).ok());
}
}  // namespace
