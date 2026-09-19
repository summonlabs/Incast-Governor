// Incast Governor - explanation construction and rendering.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include "incast/explain.hpp"

#include <algorithm>
#include <string>

#include "incast/governor.hpp"

namespace incast {
namespace {

[[nodiscard]] std::string quoted(std::string_view text) {
    std::string result("\"");
    result.append(json_escape(text));
    result.push_back('"');
    return result;
}

void append_field(std::string& buffer, std::string_view key, const std::string& value, bool& first) {
    if (!first) buffer.push_back(',');
    first = false;
    buffer.append(quoted(key));
    buffer.push_back(':');
    buffer.append(value);
}

void append_number(std::string& buffer, std::string_view key, std::uint64_t value, bool& first) {
    append_field(buffer, key, format_u64(value), first);
}

void append_signed(std::string& buffer, std::string_view key, std::int64_t value, bool& first) {
    append_field(buffer, key, std::to_string(value), first);
}

void append_string(std::string& buffer, std::string_view key, std::string_view value, bool& first) {
    append_field(buffer, key, quoted(value), first);
}

void append_boolean(std::string& buffer, std::string_view key, bool value, bool& first) {
    append_field(buffer, key, value ? std::string("true") : std::string("false"), first);
}

}  // namespace

std::string json_escape(std::string_view text) {
    std::string result;
    const std::size_t limit = std::min<std::size_t>(text.size(), kMaxIdentifierChars * 8);
    result.reserve(limit + 8);
    for (std::size_t index = 0; index < limit; ++index) {
        const unsigned char character = static_cast<unsigned char>(text[index]);
        switch (character) {
            case '"': result.append("\\\""); break;
            case '\\': result.append("\\\\"); break;
            case '\n': result.append("\\n"); break;
            case '\r': result.append("\\r"); break;
            case '\t': result.append("\\t"); break;
            default:
                if (character < 0x20) {
                    static const char* hex = "0123456789abcdef";
                    result.append("\\u00");
                    result.push_back(hex[(character >> 4) & 0x0F]);
                    result.push_back(hex[character & 0x0F]);
                } else {
                    result.push_back(static_cast<char>(character));
                }
                break;
        }
    }
    return result;
}

std::size_t Explanation::approximate_bytes() const noexcept {
    std::size_t total = headline.size() + refusal_detail.size();
    for (const auto& note : notes) total += note.size();
    for (const auto& exception : protected_exceptions) total += exception.size();
    for (const auto& contribution : top_contributors) total += contribution.disposition.size() + 96;
    total += 2048;  // fixed field surface
    return total;
}

bool Explanation::within_budget() const noexcept {
    return approximate_bytes() <= kMaxExplanationBytes && notes.size() <= kMaxExplanationNotes &&
           top_contributors.size() <= kMaxPenalizedSenders;
}

std::string Explanation::render_text() const {
    std::string text;
    text.reserve(1024);
    text.append("event=");
    text.append(format_u64(event.value()));
    text.append(" destination=");
    text.append(format_u64(destination.value()));
    text.append(" window=");
    text.append(format_u64(window.value()));
    text.append(" kind=");
    text.append(to_string(kind));
    text.append(" severity=");
    text.append(to_string(severity));
    text.append(" authoritative=");
    text.append(authoritative ? "true" : "false");
    if (refusal != RefusalReason::None) {
        text.append(" refusal=");
        text.append(to_string(refusal));
        text.append(" detail=");
        text.append(json_escape(refusal_detail));
    }
    text.push_back('\n');

    text.append("  population=");
    text.append(format_u64(metrics.active_senders));
    text.append(" synchronized=");
    text.append(format_u64(synchronization.synchronized_senders));
    text.append(" spread_ns=");
    text.append(std::to_string(synchronization.arrival_spread.nanos()));
    text.append(" sync=");
    text.append(synchronization.synchronized ? "true" : "false");
    text.append(" saturated=");
    text.append(metrics.population_saturated ? "true" : "false");
    text.push_back('\n');

    text.append("  offered_bps=");
    text.append(format_u64(metrics.aggregate_offered.bytes_per_second()));
    text.append(" capacity_bps=");
    text.append(format_u64(metrics.service_rate.bytes_per_second()));
    text.append(" oversubscription_bp=");
    text.append(format_u64(metrics.oversubscription_bp.value()));
    text.append(" queue=");
    text.append(format_u64(affected_queue.value()));
    text.append(" resource=");
    text.append(format_u64(affected_resource.value()));
    text.append(" queue_pressure_bp=");
    text.append(format_u64(metrics.queue_pressure_bp.value()));
    text.append(" occupancy=");
    text.append(format_u64(queue_occupancy.count()));
    text.append(" queue_capacity=");
    text.append(format_u64(queue_capacity.count()));
    text.append(" drops=");
    text.append(format_u64(metrics.drop_events));
    text.push_back('\n');

    text.append("  protected_offered_bps=");
    text.append(format_u64(metrics.protected_offered.bytes_per_second()));
    text.append(" protected_senders=");
    text.append(format_u64(metrics.protected_senders));
    text.append(" collisions=");
    text.append(format_u64(metrics.collided_senders));
    text.append(" penalizable_bps=");
    text.append(format_u64(metrics.penalizable_offered.bytes_per_second()));
    text.push_back('\n');

    text.append("  mitigation=");
    text.append(to_string(intent.kind));
    text.append(" scope=");
    text.append(to_string(intent.scope));
    text.append(" reduction_bp=");
    text.append(format_u64(intent.reduction_bp.value()));
    text.append(" admission_bp=");
    text.append(format_u64(intent.admission_reduction_bp.value()));
    text.append(" headroom_bytes=");
    text.append(format_u64(intent.headroom_bytes.count()));
    text.append(" stagger_slots=");
    text.append(format_u64(intent.stagger_slots));
    text.append(" penalized=");
    text.append(format_u64(intent.penalized_senders.size()));
    text.append(" duration_ns=");
    text.append(std::to_string(intent.duration.nanos()));
    text.append(" escalating=");
    text.append(intent.escalating ? "true" : "false");
    text.push_back('\n');

    text.append("  interventions: ");
    text.append(intent.active() ? (intent.continues_prior ? "continued" : "issued") : "none");
    text.append(" intervention=");
    text.append(format_u64(intent.intervention.value()));
    text.append(" generation=");
    text.append(format_u64(intent.generation.value()));
    text.push_back('\n');

    if (!protected_exceptions.empty()) {
        text.append("  protected exceptions:\n");
        for (const auto& exception : protected_exceptions) {
            text.append("    - ");
            text.append(exception);
            text.push_back('\n');
        }
    }
    if (!top_contributors.empty()) {
        text.append("  top contributors:\n");
        for (const auto& contribution : top_contributors) {
            text.append("    - sender=");
            text.append(format_u64(contribution.identity.sender.value()));
            text.append(" incarnation=");
            text.append(format_u64(contribution.identity.incarnation.value()));
            text.append(" class=");
            text.append(to_string(contribution.service));
            text.append(" rate_bps=");
            text.append(format_u64(contribution.offered_rate.bytes_per_second()));
            text.append(" share_bp=");
            text.append(format_u64(contribution.offered_share_bp.value()));
            text.append(" synchronized=");
            text.append(contribution.synchronized ? "true" : "false");
            text.append(" disposition=");
            text.append(contribution.disposition);
            text.push_back('\n');
        }
    }
    text.append("  authority: ");
    text.append(authority.summary());
    text.push_back('\n');
    if (!notes.empty()) {
        text.append("  notes:\n");
        for (const auto& note : notes) {
            text.append("    - ");
            text.append(note);
            text.push_back('\n');
        }
    }
    return text;
}

std::string Explanation::render_json() const {
    std::string json;
    json.reserve(2048);
    json.push_back('{');
    bool first = true;

    append_number(json, "event", event.value(), first);
    append_number(json, "event_generation", event_generation.value(), first);
    append_number(json, "destination", destination.value(), first);
    append_number(json, "window", window.value(), first);
    append_number(json, "window_generation", window_generation.value(), first);
    append_number(json, "epoch", epoch.value(), first);
    append_number(json, "boot", boot.value(), first);
    append_number(json, "worker", worker.value(), first);
    append_number(json, "lease", lease.value(), first);
    append_number(json, "policy", policy.value(), first);
    append_number(json, "policy_generation", policy_generation.value(), first);
    append_signed(json, "decided_at_ns", decided_at.nanos(), first);
    append_string(json, "kind", to_string(kind), first);
    append_string(json, "severity", to_string(severity), first);
    append_boolean(json, "authoritative", authoritative, first);
    append_string(json, "refusal", to_string(refusal), first);
    append_string(json, "refusal_detail", refusal_detail, first);
    append_string(json, "headline", headline, first);

    append_number(json, "active_senders", metrics.active_senders, first);
    append_number(json, "distinct_flows", metrics.distinct_flows, first);
    append_boolean(json, "population_saturated", metrics.population_saturated, first);
    append_number(json, "protected_senders", metrics.protected_senders, first);
    append_number(json, "collided_senders", metrics.collided_senders, first);
    append_signed(json, "arrival_spread_ns", metrics.arrival_spread.nanos(), first);
    append_signed(json, "window_width_ns", metrics.window_width.nanos(), first);
    append_number(json, "aggregate_offered_bps", metrics.aggregate_offered.bytes_per_second(), first);
    append_number(json, "protected_offered_bps", metrics.protected_offered.bytes_per_second(), first);
    append_number(json, "penalizable_offered_bps", metrics.penalizable_offered.bytes_per_second(), first);
    append_number(json, "service_rate_bps", metrics.service_rate.bytes_per_second(), first);
    append_number(json, "oversubscription_bp", metrics.oversubscription_bp.value(), first);
    append_number(json, "queue_pressure_bp", metrics.queue_pressure_bp.value(), first);
    append_number(json, "queue_occupancy", metrics.queue_occupancy.count(), first);
    append_number(json, "queue_capacity", metrics.queue_capacity.count(), first);
    append_number(json, "buffer_headroom", metrics.buffer_headroom.count(), first);
    append_number(json, "drop_events", metrics.drop_events, first);
    append_number(json, "ecn_marks", metrics.ecn_marks, first);

    append_boolean(json, "synchronization_evaluated", synchronization.evaluated, first);
    append_boolean(json, "synchronized", synchronization.synchronized, first);
    append_number(json, "synchronized_senders", synchronization.synchronized_senders, first);
    append_number(json, "arrival_compression_bp", synchronization.arrival_compression_bp.value(), first);
    append_number(json, "synchronized_share_bp", synchronization.synchronized_share_bp.value(), first);
    append_signed(json, "sync_window_bound_ns", synchronization.sync_window_bound.nanos(), first);

    append_number(json, "affected_queue", affected_queue.value(), first);
    append_number(json, "affected_resource", affected_resource.value(), first);

    append_string(json, "mitigation", to_string(intent.kind), first);
    append_string(json, "mitigation_scope", to_string(intent.scope), first);
    append_number(json, "intervention", intent.intervention.value(), first);
    append_number(json, "intervention_generation", intent.generation.value(), first);
    append_number(json, "reduction_bp", intent.reduction_bp.value(), first);
    append_number(json, "admission_reduction_bp", intent.admission_reduction_bp.value(), first);
    append_number(json, "aggregate_reduction_bp", intent.aggregate_reduction_bp.value(), first);
    append_number(json, "headroom_bytes", intent.headroom_bytes.count(), first);
    append_number(json, "stagger_slots", intent.stagger_slots, first);
    append_number(json, "penalized_senders", intent.penalized_senders.size(), first);
    append_signed(json, "intent_duration_ns", intent.duration.nanos(), first);
    append_boolean(json, "intent_escalating", intent.escalating, first);
    append_boolean(json, "intent_insufficient", intent.insufficient, first);
    append_boolean(json, "intent_clamped", intent.clamped, first);
    append_boolean(json, "intent_relaxes_prior", intent.relaxes_prior, first);
    append_boolean(json, "intent_continues_prior", intent.continues_prior, first);
    append_number(json, "protected_floor_bps", intent.protected_floor.bytes_per_second(), first);

    append_boolean(json, "authority_authorized", authority.intervention_authorized, first);
    append_string(json, "authority_refusal", to_string(authority.refusal), first);
    append_boolean(json, "authority_population_saturated", authority.population_saturated, first);
    append_boolean(json, "authority_identity_collision", authority.identity_collision_present, first);
    append_boolean(json, "authority_protected_floor_preserved", authority.protected_floor_preserved, first);
    append_boolean(json, "authority_recovery_policy_present", authority.recovery_policy_present, first);
    append_boolean(json, "authority_clamped", authority.clamped, first);

    if (!first) json.push_back(',');
    first = false;
    json.append("\"evidence_classes\":{");
    bool inner_first = true;
    for (std::size_t index = 0; index < kEvidenceClassCount; ++index) {
        append_string(json, to_string(static_cast<EvidenceClass>(index)),
                      to_string(authority.classes[index]), inner_first);
    }
    json.push_back('}');

    if (!first) json.push_back(',');
    first = false;
    json.append("\"protected_exceptions\":[");
    for (std::size_t index = 0; index < protected_exceptions.size(); ++index) {
        if (index != 0) json.push_back(',');
        json.append(quoted(protected_exceptions[index]));
    }
    json.push_back(']');

    if (!first) json.push_back(',');
    first = false;
    json.append("\"notes\":[");
    for (std::size_t index = 0; index < notes.size(); ++index) {
        if (index != 0) json.push_back(',');
        json.append(quoted(notes[index]));
    }
    json.push_back(']');

    if (!first) json.push_back(',');
    first = false;
    json.append("\"top_contributors\":[");
    for (std::size_t index = 0; index < top_contributors.size(); ++index) {
        const auto& contribution = top_contributors[index];
        if (index != 0) json.push_back(',');
        json.push_back('{');
        bool contribution_first = true;
        append_number(json, "sender", contribution.identity.sender.value(), contribution_first);
        append_number(json, "incarnation", contribution.identity.incarnation.value(), contribution_first);
        append_number(json, "flow", contribution.identity.flow.value(), contribution_first);
        append_string(json, "service", to_string(contribution.service), contribution_first);
        append_boolean(json, "protected", contribution.protected_obligation, contribution_first);
        append_boolean(json, "collided", contribution.collided, contribution_first);
        append_boolean(json, "synchronized", contribution.synchronized, contribution_first);
        append_boolean(json, "mitigable", contribution.mitigable, contribution_first);
        append_number(json, "offered_bps", contribution.offered_rate.bytes_per_second(), contribution_first);
        append_number(json, "offered_share_bp", contribution.offered_share_bp.value(), contribution_first);
        append_number(json, "capacity_share_bp", contribution.aggregate_share_bp.value(), contribution_first);
        append_signed(json, "arrival_offset_ns", contribution.arrival_offset.nanos(), contribution_first);
        append_string(json, "disposition", contribution.disposition, contribution_first);
        json.push_back('}');
    }
    json.push_back(']');
    json.push_back('}');
    return json;
}

Explanation build_explanation(const Decision& decision,
                              const std::vector<SenderContribution>& all_contributions,
                              std::size_t max_contributors) {
    Explanation explanation{};
    explanation.event = decision.event;
    explanation.event_generation = decision.event_generation;
    explanation.destination = decision.destination;
    explanation.window = decision.window;
    explanation.window_generation = decision.window_generation;
    explanation.epoch = decision.epoch;
    explanation.boot = decision.boot;
    explanation.worker = decision.worker;
    explanation.lease = decision.lease;
    explanation.policy = decision.policy;
    explanation.policy_generation = decision.policy_generation;
    explanation.decided_at = decision.decided_at;
    explanation.kind = decision.kind;
    explanation.severity = decision.severity;
    explanation.authoritative = decision.kind != IncidentKind::Unknown;
    explanation.refusal = decision.refusal;
    explanation.refusal_detail = decision.refusal_detail;
    explanation.metrics = decision.metrics;
    explanation.synchronization = decision.synchronization;
    explanation.authority = decision.authority;
    explanation.intent = decision.intent;
    explanation.notes = decision.explanation.notes;
    if (explanation.notes.size() > kMaxExplanationNotes) explanation.notes.resize(kMaxExplanationNotes);

    explanation.affected_queue = decision.intent.queue;
    explanation.affected_resource = decision.intent.resource;
    explanation.queue_occupancy = decision.metrics.queue_occupancy;
    explanation.queue_capacity = decision.metrics.queue_capacity;
    explanation.buffer_headroom = decision.metrics.buffer_headroom;

    const std::size_t limit = std::min(max_contributors, decision.contributions.size());
    explanation.top_contributors.assign(decision.contributions.begin(),
                                       decision.contributions.begin() + static_cast<std::ptrdiff_t>(limit));

    for (const auto& contribution : all_contributions) {
        if (!contribution.protected_obligation && !contribution.collided) continue;
        std::string exception("sender=");
        exception.append(format_u64(contribution.identity.sender.value()));
        exception.append(" incarnation=");
        exception.append(format_u64(contribution.identity.incarnation.value()));
        exception.append(" class=");
        exception.append(to_string(contribution.service));
        exception.append(" reason=");
        exception.append(contribution.protected_obligation ? "protected-obligation" : "identity-collision");
        if (explanation.protected_exceptions.size() < kMaxExplanationNotes) {
            explanation.protected_exceptions.push_back(std::move(exception));
        }
    }

    std::string headline(to_string(decision.kind));
    headline.append(" severity=");
    headline.append(to_string(decision.severity));
    headline.append(" senders=");
    headline.append(format_u64(decision.metrics.active_senders));
    headline.append(" offered_bps=");
    headline.append(format_u64(decision.metrics.aggregate_offered.bytes_per_second()));
    headline.append(" capacity_bps=");
    headline.append(format_u64(decision.metrics.service_rate.bytes_per_second()));
    headline.append(" oversubscription_bp=");
    headline.append(format_u64(decision.metrics.oversubscription_bp.value()));
    headline.append(" outcome=");
    headline.append(to_string(decision.outcome));
    headline.append(" mitigation=");
    headline.append(to_string(decision.intent.kind));
    headline.append(" reduction_bp=");
    headline.append(format_u64(decision.intent.reduction_bp.value()));
    headline.append(" authorized=");
    headline.append(decision.authority.intervention_authorized ? "true" : "false");
    headline.append(" refusal=");
    headline.append(to_string(decision.authority.refusal));
    if (headline.size() > 512) headline.resize(512);
    explanation.headline = std::move(headline);

    if (explanation.notes.empty() && decision.kind == IncidentKind::Unknown) {
        explanation.notes.push_back(std::string("classification is UNKNOWN: ") +
                                    std::string(to_string(decision.refusal)));
    }
    return explanation;
}

}  // namespace incast
