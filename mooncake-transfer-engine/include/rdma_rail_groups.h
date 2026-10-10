// Copyright 2026 KVCache.AI
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy at http://www.apache.org/licenses/LICENSE-2.0

#ifndef MOONCAKE_RDMA_RAIL_GROUPS_H
#define MOONCAKE_RDMA_RAIL_GROUPS_H

#include <arpa/inet.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdint>
#include <sstream>
#include <string>
#include <vector>

namespace mooncake {

// Operator-declared connectivity groups, not inferred routing information.
// IPv4 CIDRs refer to IPv4-mapped RoCE GIDs. No device-name convention is used.
// MC_RDMA_RAIL_GROUPS="10.80.20.0/24;10.80.21.0/24" permits all HCAs within
// either declared group, but never pairs HCAs across the groups. Unset keeps
// the legacy full-mesh policy; empty/invalid/overlapping input is rejected.
class RdmaRailGroups {
   public:
    bool configure(const char *value) {
        enabled_ = value != nullptr;
        groups_.clear();
        if (!enabled_) return true;
        std::string input(value);
        if (input.empty() || input.back() == ';') return false;
        std::istringstream stream(input);
        std::string token;
        std::vector<Network> parsed;
        while (std::getline(stream, token, ';')) {
            auto begin = token.find_first_not_of(" \t\r\n");
            if (begin == std::string::npos) return false;
            token = token.substr(begin,
                                 token.find_last_not_of(" \t\r\n") - begin + 1);
            auto slash = token.find('/');
            if (slash == std::string::npos) return false;
            Network network;
            auto prefix = token.substr(slash + 1);
            auto result = std::from_chars(
                prefix.data(), prefix.data() + prefix.size(), network.bits);
            if (result.ec != std::errc{} ||
                result.ptr != prefix.data() + prefix.size())
                return false;
            auto address = token.substr(0, slash);
            if (inet_pton(AF_INET, address.c_str(),
                          network.bytes.data() + 12) == 1) {
                if (network.bits < 0 || network.bits > 32) return false;
                network.bytes[10] = network.bytes[11] = 0xff;
                network.bits += 96;
            } else if (inet_pton(AF_INET6, address.c_str(),
                                 network.bytes.data()) != 1 ||
                       network.bits < 0 || network.bits > 128) {
                return false;
            }
            for (const auto &other : parsed) {
                if (samePrefix(network.bytes, other.bytes,
                               std::min(network.bits, other.bits)))
                    return false;
            }
            parsed.push_back(network);
        }
        groups_ = std::move(parsed);
        return !groups_.empty();
    }

    bool enabled() const { return enabled_; }

    bool allows(const std::string &local_gid,
                const std::string &peer_gid) const {
        if (!enabled_) return true;
        int local = groupForGid(local_gid);
        return local >= 0 && local == groupForGid(peer_gid);
    }

    int groupForGid(const std::string &gid) const {
        // Mooncake metadata uses 16 colon-separated bytes, not IPv6 text.
        std::array<uint8_t, 16> bytes{};
        const char *cursor = gid.data();
        const char *end = cursor + gid.size();
        for (size_t i = 0; i < bytes.size(); ++i) {
            unsigned value;
            auto result = std::from_chars(cursor, end, value, 16);
            if (result.ec != std::errc{} || value > 255) return -1;
            bytes[i] = static_cast<uint8_t>(value);
            cursor = result.ptr;
            if (i + 1 < bytes.size()) {
                if (cursor == end || *cursor++ != ':') return -1;
            }
        }
        if (cursor != end) return -1;
        for (size_t i = 0; i < groups_.size(); ++i) {
            if (samePrefix(bytes, groups_[i].bytes, groups_[i].bits))
                return static_cast<int>(i);
        }
        return -1;
    }

   private:
    struct Network {
        std::array<uint8_t, 16> bytes{};
        int bits = 0;
    };
    static bool samePrefix(const std::array<uint8_t, 16> &a,
                           const std::array<uint8_t, 16> &b, int bits) {
        int whole = bits / 8;
        if (!std::equal(a.begin(), a.begin() + whole, b.begin())) return false;
        int remainder = bits % 8;
        return !remainder ||
               ((a[whole] ^ b[whole]) & (0xff << (8 - remainder))) == 0;
    }
    bool enabled_ = false;
    std::vector<Network> groups_;
};

}  // namespace mooncake
#endif
