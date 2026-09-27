// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// The canonical snapshot payload codec.
//
// The payload is the authoritative state encoded with the canonical writer.
// The layout below is version 1.0 and is append-only within a major version:
// a field is never reordered, removed, or given a different meaning without a
// major version change.
//
//   u16    format_major
//   u16    format_minor
//   varint registry_generation
//   u8     has_external_epoch
//          varint epoch_value
//          text   epoch_issuer
//   varint last_sequence
//   varint history_dropped
//   varint limits, in declaration order:
//          max_records, max_query_results, max_aliases_per_record,
//          max_memberships_per_record, max_metadata_entries,
//          max_metadata_value_bytes, max_display_name_bytes, max_address_lines,
//          max_address_line_bytes, max_provenance_detail_bytes,
//          max_history_entries, max_idempotency_entries, max_snapshot_bytes
//   varint record_count
//          records, ascending by canonical identity
//   varint history_count
//          history entries, ascending by sequence
//   varint idempotency_count
//          idempotency entries, ascending by key
//
// Decoding is a full revalidation: types, syntax, enum domains, ordering,
// lengths and cross-field coupling are all checked, so a payload that was
// tampered with by someone able to recompute the digests is still rejected
// unless it is a genuinely valid state.

#ifndef DCR_CORE_CODEC_HPP
#define DCR_CORE_CODEC_HPP

#include <string>
#include <string_view>

#include "core/state.hpp"
#include "dcr/limits.hpp"
#include "dcr/result.hpp"

namespace dcr::internal {

/// Encodes a state into its canonical payload. Fails when the state is invalid
/// or the payload would exceed the state's own snapshot bound.
[[nodiscard]] Result<std::string> encode_state(const RegistryState& state);

/// Decodes and fully validates a canonical payload.
///
/// Every bound that applies is the tighter of the persisted bound, the
/// configured bound and the structural maximum, so a store cannot widen its
/// own limits. The returned state carries the configured limits, which is what
/// the next commit will persist.
[[nodiscard]] Result<RegistryState> decode_state(std::string_view payload,
                                                 const RegistryLimits& configured_limits);

/// The tighter of two limit sets, field by field, additionally clamped to the
/// structural maxima.
[[nodiscard]] RegistryLimits tighten_limits(const RegistryLimits& left,
                                            const RegistryLimits& right) noexcept;

}  // namespace dcr::internal

#endif  // DCR_CORE_CODEC_HPP
