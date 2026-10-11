#pragma once

#include <glog/logging.h>

#include <cstddef>
#include <map>
#include <string_view>

#include "types.h"

namespace mooncake {

// One diagnostic per error category, never per key. Retain only the first
// index as a bounded sample; arbitrarily long object keys are not copied.
class DiskEvictionErrorSummary {
   public:
    void Record(ErrorCode error, size_t index) {
        if (error == ErrorCode::OBJECT_NOT_FOUND) {
            ++missing_count_;
            return;
        }
        auto it = errors_.try_emplace(error, Counts{0, index}).first;
        ++it->second.count;
    }

    void Log(std::string_view operation, size_t total_keys) const {
        if (missing_count_ != 0) {
            VLOG(1) << operation << ": already absent=" << missing_count_
                    << ", total_keys=" << total_keys;
        }
        for (const auto& [error, counts] : errors_) {
            LOG(WARNING) << operation << ": error=" << toString(error)
                         << ", count=" << counts.count
                         << ", total_keys=" << total_keys
                         << ", first_key_index=" << counts.first_index;
        }
    }

   private:
    struct Counts {
        size_t count;
        size_t first_index;
    };
    size_t missing_count_ = 0;
    std::map<ErrorCode, Counts> errors_;
};

}  // namespace mooncake
