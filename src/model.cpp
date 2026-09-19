// Incast Governor - evidence model implementation.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include "incast/model.hpp"

#include <algorithm>
#include <chrono>

namespace incast {

Timestamp monotonic_now() noexcept {
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    const auto nanos = std::chrono::duration_cast<std::chrono::nanoseconds>(now).count();
    // 0 is reserved for "absent"; a genuine zero reading is remapped to 1.
    return Timestamp{nanos == 0 ? 1 : nanos};
}

std::optional<ServiceClass> parse_service_class(std::string_view text) noexcept {
    if (text == "BestEffort") return ServiceClass::BestEffort;
    if (text == "Bulk") return ServiceClass::Bulk;
    if (text == "Interactive") return ServiceClass::Interactive;
    if (text == "LatencySensitive") return ServiceClass::LatencySensitive;
    if (text == "Control") return ServiceClass::Control;
    if (text == "Replication") return ServiceClass::Replication;
    return std::nullopt;
}

bool EvidenceBundle::structurally_valid() const noexcept {
    if (destination.absent()) return false;
    if (window.absent()) return false;
    if (event.absent()) return false;
    if (!now.valid()) return false;
    if (window_start.valid() && window_end.valid() && window_end < window_start) return false;
    if (senders.size() > kMaxSendersPerWindow) return false;
    if (queues.size() > kMaxQueueObservations) return false;
    return true;
}

}  // namespace incast
