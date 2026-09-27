// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "core/codec.hpp"

#include <algorithm>
#include <array>

#include "core/canonical.hpp"
#include "dcr/version.hpp"
#include "internal/text.hpp"

namespace dcr::internal {
namespace {

[[nodiscard]] Error corrupt(std::string message, const char* field) {
  return Error(ErrorCode::store_corrupt, std::move(message), field);
}

/// Wraps a rejection that came from decoding or validating payload content.
///
/// Most such rejections mean the payload is corrupt. Two categories do not:
/// content that exceeds a configured bound is a limit problem, and a payload
/// written by a newer format is a version problem. Both are more actionable
/// than "corrupt", so both are preserved. A bound violation is raised as
/// `limit_exceeded` while decoding and is reported as `store_limit_exceeded`,
/// which is the category a caller of a store operation branches on.
[[nodiscard]] Error wrap_payload_error(const Error& inner, std::string context) {
  switch (inner.code()) {
    case ErrorCode::limit_exceeded:
    case ErrorCode::store_limit_exceeded:
      return Error(ErrorCode::store_limit_exceeded, std::move(context) + ": " + inner.message(),
                   inner.field());
    case ErrorCode::store_incompatible_version:
      return Error(ErrorCode::store_incompatible_version,
                   std::move(context) + ": " + inner.message(), inner.field());
    default:
      return Error(ErrorCode::store_corrupt, std::move(context) + ": " + inner.message(),
                   inner.field());
  }
}

// ---------------------------------------------------------------------------
// Encoding
// ---------------------------------------------------------------------------

void write_optional_text(CanonicalWriter& writer, bool present, std::string_view text) {
  writer.boolean(present);
  if (present) {
    writer.text(text);
  }
}

void write_metadata_map(CanonicalWriter& writer, const MetadataMap& map) {
  writer.varint(map.entries().size());
  for (const auto& entry : map.entries()) {
    writer.text(entry.key.value());
    writer.text(entry.value);
  }
}

void write_facility(CanonicalWriter& writer, const FacilityMetadata& facility) {
  write_optional_text(writer, facility.country.has_value(),
                      facility.country.has_value() ? facility.country->value() : std::string_view());
  write_optional_text(writer, facility.region.has_value(),
                      facility.region.has_value() ? facility.region->value() : std::string_view());
  write_optional_text(writer, facility.metro.has_value(),
                      facility.metro.has_value() ? facility.metro->value() : std::string_view());

  writer.boolean(facility.address.has_value());
  if (facility.address.has_value()) {
    writer.varint(facility.address->lines().size());
    for (const auto& line : facility.address->lines()) {
      writer.text(line);
    }
    writer.text(facility.address->locality());
    writer.text(facility.address->administrative_area());
    writer.text(facility.address->postal_code());
  }

  writer.boolean(facility.coordinates.has_value());
  if (facility.coordinates.has_value()) {
    writer.i32(facility.coordinates->latitude_e7());
    writer.i32(facility.coordinates->longitude_e7());
  }

  writer.boolean(facility.tier.has_value());
  if (facility.tier.has_value()) {
    writer.u8(static_cast<std::uint8_t>(*facility.tier));
  }

  write_optional_text(writer, facility.facility_operator.has_value(),
                      facility.facility_operator.has_value() ? facility.facility_operator->value()
                                                             : std::string_view());
  write_metadata_map(writer, facility.external_refs);
}

void write_provenance(CanonicalWriter& writer, const ProvenanceRecord& provenance) {
  writer.u8(static_cast<std::uint8_t>(provenance.source()));
  writer.text(provenance.principal().value());
  writer.varint(provenance.sequence().value());
  writer.varint(provenance.generation().value());
  writer.i64(provenance.recorded_at().unix_millis());
  writer.boolean(provenance.epoch().has_value());
  if (provenance.epoch().has_value()) {
    writer.varint(provenance.epoch()->value());
    writer.text(provenance.epoch()->issuer().value());
  }
  writer.boolean(provenance.reason().has_value());
  if (provenance.reason().has_value()) {
    writer.text(provenance.reason()->value());
  }
  writer.text(provenance.detail());
}

void write_record(CanonicalWriter& writer, const DataCenterRecord& record) {
  writer.text(record.id.value());
  writer.varint(record.revision.value());
  writer.text(record.display_name);

  writer.varint(record.aliases.size());
  for (const auto& alias : record.aliases) {
    writer.text(alias.value());
  }

  writer.varint(record.memberships.size());
  for (const auto& membership : record.memberships) {
    writer.text(membership.site.value());
    writer.u8(static_cast<std::uint8_t>(membership.role));
  }

  write_facility(writer, record.facility);
  write_metadata_map(writer, record.extensions);

  writer.u8(static_cast<std::uint8_t>(record.ownership.kind()));
  writer.boolean(record.ownership.scope().has_value());
  if (record.ownership.scope().has_value()) {
    writer.text(record.ownership.scope()->value());
  }

  writer.u16(record.compatibility.model_major());
  writer.u16(record.compatibility.model_minor());
  writer.u64(record.compatibility.capability_mask());

  writer.u8(static_cast<std::uint8_t>(record.state));
  writer.varint(record.created_generation.value());

  writer.boolean(record.retired_generation.has_value());
  if (record.retired_generation.has_value()) {
    writer.varint(record.retired_generation->value());
  }

  writer.boolean(record.retirement.has_value());
  if (record.retirement.has_value()) {
    writer.boolean(record.retirement->reason.has_value());
    if (record.retirement->reason.has_value()) {
      writer.text(record.retirement->reason->value());
    }
    writer.varint(record.retirement->generation.value());
    writer.boolean(record.retirement->replaced_by.has_value());
    if (record.retirement->replaced_by.has_value()) {
      writer.text(record.retirement->replaced_by->value());
    }
  }

  writer.boolean(record.replaces.has_value());
  if (record.replaces.has_value()) {
    writer.text(record.replaces->value());
  }

  write_provenance(writer, record.last_provenance);
  writer.varint(record.last_sequence.value());
}

void write_history(CanonicalWriter& writer, const HistoryEntry& entry) {
  writer.varint(entry.sequence.value());
  writer.varint(entry.generation.value());
  writer.text(entry.id.value());
  writer.u8(static_cast<std::uint8_t>(entry.action));
  writer.varint(entry.revision.value());
  writer.boolean(entry.previous_state.has_value());
  if (entry.previous_state.has_value()) {
    writer.u8(static_cast<std::uint8_t>(*entry.previous_state));
  }
  writer.u8(static_cast<std::uint8_t>(entry.new_state));
  write_provenance(writer, entry.provenance);
}

void write_idempotency(CanonicalWriter& writer, const IdempotencyEntry& entry) {
  writer.text(entry.key.value());
  writer.text(std::string_view(reinterpret_cast<const char*>(entry.command_digest.bytes().data()),
                               Sha256Digest::kSize));
  writer.u8(static_cast<std::uint8_t>(entry.status));
  writer.text(entry.id.value());
  writer.varint(entry.generation.value());
  writer.varint(entry.revision.value());
  writer.boolean(entry.related_id.has_value());
  if (entry.related_id.has_value()) {
    writer.text(entry.related_id->value());
  }
  writer.boolean(entry.sequence.has_value());
  if (entry.sequence.has_value()) {
    writer.varint(entry.sequence->value());
  }
  writer.varint(entry.recorded_sequence.value());
}

void write_limits(CanonicalWriter& writer, const RegistryLimits& limits) {
  writer.varint(limits.max_records);
  writer.varint(limits.max_query_results);
  writer.varint(limits.max_aliases_per_record);
  writer.varint(limits.max_memberships_per_record);
  writer.varint(limits.max_metadata_entries);
  writer.varint(limits.max_metadata_value_bytes);
  writer.varint(limits.max_display_name_bytes);
  writer.varint(limits.max_address_lines);
  writer.varint(limits.max_address_line_bytes);
  writer.varint(limits.max_provenance_detail_bytes);
  writer.varint(limits.max_history_entries);
  writer.varint(limits.max_idempotency_entries);
  writer.varint(limits.max_snapshot_bytes);
}

// ---------------------------------------------------------------------------
// Decoding
// ---------------------------------------------------------------------------

template <class Token>
[[nodiscard]] Result<Token> read_token(CanonicalReader& reader, const char* field) {
  DCR_TRY_ASSIGN(const std::string_view text, reader.text());
  auto parsed = Token::parse(text);
  if (!parsed.has_value()) {
    return wrap_payload_error(parsed.error(),
                              std::string(field) + " in the payload is invalid");
  }
  return std::move(parsed).value();
}

template <class Enum>
[[nodiscard]] Result<Enum> read_enum(CanonicalReader& reader, std::uint8_t minimum,
                                     std::uint8_t maximum, const char* field) {
  DCR_TRY_ASSIGN(const std::uint8_t value, reader.u8());
  if (value < minimum || value > maximum) {
    return corrupt(std::string(field) + " holds " + std::to_string(value) +
                       ", which is not a valid " + std::string(field),
                   field);
  }
  return static_cast<Enum>(value);
}

[[nodiscard]] Result<std::string> read_text(CanonicalReader& reader, std::size_t maximum,
                                            const char* field, bool allow_empty) {
  DCR_TRY_ASSIGN(const std::string_view view, reader.text());
  auto validated = validate_text(view, maximum, field, allow_empty);
  if (!validated.has_value()) {
    return wrap_payload_error(validated.error(),
                              std::string(field) + " in the payload is invalid");
  }
  return std::move(validated).value();
}

[[nodiscard]] Result<std::optional<std::string>> read_optional_text(CanonicalReader& reader,
                                                                    std::size_t maximum,
                                                                    const char* field) {
  DCR_TRY_ASSIGN(const bool present, reader.boolean());
  if (!present) {
    return std::optional<std::string>();
  }
  DCR_TRY_ASSIGN(std::string text, read_text(reader, maximum, field, false));
  return std::optional<std::string>(std::move(text));
}

[[nodiscard]] Result<RegistryGeneration> read_generation(CanonicalReader& reader,
                                                         const char* field) {
  DCR_TRY_ASSIGN(const std::uint64_t value, reader.varint());
  auto generation = RegistryGeneration::from_value(value);
  if (!generation.has_value()) {
    return corrupt(std::string(field) + " in the payload is out of range", field);
  }
  return generation.value();
}

[[nodiscard]] Result<MetadataRevision> read_revision(CanonicalReader& reader, const char* field) {
  DCR_TRY_ASSIGN(const std::uint64_t value, reader.varint());
  auto revision = MetadataRevision::from_value(value);
  if (!revision.has_value()) {
    return corrupt(std::string(field) + " in the payload is out of range", field);
  }
  return revision.value();
}

[[nodiscard]] Result<SequenceNumber> read_sequence(CanonicalReader& reader, const char* field) {
  DCR_TRY_ASSIGN(const std::uint64_t value, reader.varint());
  auto sequence = SequenceNumber::from_value(value);
  if (!sequence.has_value()) {
    return corrupt(std::string(field) + " in the payload is out of range", field);
  }
  return sequence.value();
}

/// Reads a varint that must fit a 32-bit field, so that narrowing it cannot
/// silently lose data.
[[nodiscard]] Result<std::uint32_t> read_u32_varint(CanonicalReader& reader, const char* field) {
  DCR_TRY_ASSIGN(const std::uint64_t value, reader.varint());
  if (value > 0xFFFFFFFFULL) {
    return corrupt(std::string(field) + " in the payload exceeds 32 bits", field);
  }
  return static_cast<std::uint32_t>(value);
}

[[nodiscard]] Result<MetadataMap> read_metadata_map(CanonicalReader& reader,
                                                    const RegistryLimits& limits,
                                                    const char* field) {
  DCR_TRY_ASSIGN(const std::size_t count,
                 reader.count(field, StructuralLimits::kMaxMetadataEntries));
  std::vector<MetadataEntry> entries;
  entries.reserve(count);
  for (std::size_t index = 0; index < count; ++index) {
    DCR_TRY_ASSIGN(MetadataKey key, read_token<MetadataKey>(reader, "metadata key"));
    DCR_TRY_ASSIGN(std::string value,
                   read_text(reader, StructuralLimits::kMaxMetadataValueBytes, field, true));
    entries.push_back(MetadataEntry{std::move(key), std::move(value)});
  }
  auto map = MetadataMap::make(std::move(entries), field);
  if (!map.has_value()) {
    return Error(ErrorCode::store_corrupt,
                 std::string(field) + " in the payload is invalid: " + map.error().message(),
                 field);
  }
  MetadataMap result = std::move(map).value();
  auto validation = result.validate(limits, field);
  if (!validation.has_value()) {
    return wrap_payload_error(validation.error(),
                              std::string(field) + " in the payload is invalid");
  }
  return result;
}

[[nodiscard]] Result<FacilityMetadata> read_facility(CanonicalReader& reader,
                                                     const RegistryLimits& limits) {
  FacilityMetadata facility;
  DCR_TRY_ASSIGN(const auto country, read_optional_text(reader, 2, "facility.country"));
  if (country.has_value()) {
    DCR_TRY_ASSIGN(CountryCode parsed, CountryCode::parse(*country));
    facility.country = std::move(parsed);
  }
  DCR_TRY_ASSIGN(const auto region, read_optional_text(reader, 32, "facility.region"));
  if (region.has_value()) {
    DCR_TRY_ASSIGN(RegionCode parsed, RegionCode::parse(*region));
    facility.region = std::move(parsed);
  }
  DCR_TRY_ASSIGN(const auto metro, read_optional_text(reader, 32, "facility.metro"));
  if (metro.has_value()) {
    DCR_TRY_ASSIGN(MetroCode parsed, MetroCode::parse(*metro));
    facility.metro = std::move(parsed);
  }

  DCR_TRY_ASSIGN(const bool has_address, reader.boolean());
  if (has_address) {
    DCR_TRY_ASSIGN(const std::size_t line_count,
                   reader.count("facility.address.lines", StructuralLimits::kMaxAddressLines));
    std::vector<std::string> lines;
    lines.reserve(line_count);
    for (std::size_t index = 0; index < line_count; ++index) {
      DCR_TRY_ASSIGN(std::string line,
                     read_text(reader, StructuralLimits::kMaxAddressLineBytes,
                               "facility.address.lines", false));
      lines.push_back(std::move(line));
    }
    DCR_TRY_ASSIGN(std::string locality,
                   read_text(reader, StructuralLimits::kMaxAddressLineBytes,
                             "facility.address.locality", true));
    DCR_TRY_ASSIGN(std::string area,
                   read_text(reader, StructuralLimits::kMaxAddressLineBytes,
                             "facility.address.administrative_area", true));
    DCR_TRY_ASSIGN(std::string postal,
                   read_text(reader, StructuralLimits::kMaxAddressLineBytes,
                             "facility.address.postal_code", true));
    auto address = PostalAddress::make(std::move(lines), locality, area, postal);
    if (!address.has_value()) {
      return Error(ErrorCode::store_corrupt,
                   "facility.address in the payload is invalid: " + address.error().message(),
                   "facility.address");
    }
    facility.address = std::move(address).value();
  }

  DCR_TRY_ASSIGN(const bool has_coordinates, reader.boolean());
  if (has_coordinates) {
    DCR_TRY_ASSIGN(const std::int32_t latitude, reader.i32());
    DCR_TRY_ASSIGN(const std::int32_t longitude, reader.i32());
    auto coordinates = GeoCoordinates::make(latitude, longitude);
    if (!coordinates.has_value()) {
      return Error(ErrorCode::store_corrupt,
                   "facility.coordinates in the payload is invalid: " +
                       coordinates.error().message(),
                   "facility.coordinates");
    }
    facility.coordinates = coordinates.value();
  }

  DCR_TRY_ASSIGN(const bool has_tier, reader.boolean());
  if (has_tier) {
    DCR_TRY_ASSIGN(const FacilityTier tier,
                   read_enum<FacilityTier>(reader, 1, 4, "facility.tier"));
    facility.tier = tier;
  }

  DCR_TRY_ASSIGN(const auto facility_operator,
                 read_optional_text(reader, 128, "facility.facility_operator"));
  if (facility_operator.has_value()) {
    DCR_TRY_ASSIGN(PrincipalId parsed, PrincipalId::parse(*facility_operator));
    facility.facility_operator = std::move(parsed);
  }

  DCR_TRY_ASSIGN(MetadataMap refs, read_metadata_map(reader, limits, "facility.external_refs"));
  facility.external_refs = std::move(refs);

  auto validation = facility.validate(limits, "facility");
  if (!validation.has_value()) {
    return wrap_payload_error(validation.error(), "facility in the payload is invalid");
  }
  return facility;
}

[[nodiscard]] Result<ProvenanceRecord> read_provenance(CanonicalReader& reader,
                                                       const RegistryLimits& limits) {
  DCR_TRY_ASSIGN(const ProvenanceSource source,
                 read_enum<ProvenanceSource>(reader, 1, 6, "provenance.source"));
  DCR_TRY_ASSIGN(PrincipalId principal, read_token<PrincipalId>(reader, "provenance.principal"));
  DCR_TRY_ASSIGN(const SequenceNumber sequence, read_sequence(reader, "provenance.sequence"));
  DCR_TRY_ASSIGN(const RegistryGeneration generation,
                 read_generation(reader, "provenance.generation"));
  DCR_TRY_ASSIGN(const std::int64_t recorded_at, reader.i64());
  auto timestamp = Timestamp::from_unix_millis(recorded_at);
  if (!timestamp.has_value()) {
    return corrupt("provenance.recorded_at in the payload is out of range",
                   "provenance.recorded_at");
  }

  std::optional<EpochToken> epoch;
  DCR_TRY_ASSIGN(const bool has_epoch, reader.boolean());
  if (has_epoch) {
    DCR_TRY_ASSIGN(const std::uint64_t value, reader.varint());
    DCR_TRY_ASSIGN(EpochIssuerId issuer, read_token<EpochIssuerId>(reader, "provenance.epoch"));
    epoch = EpochToken::make(std::move(issuer), value).value();
  }

  std::optional<ReasonCode> reason;
  DCR_TRY_ASSIGN(const bool has_reason, reader.boolean());
  if (has_reason) {
    reason = read_token<ReasonCode>(reader, "provenance.reason").value();
  }

  DCR_TRY_ASSIGN(std::string detail,
                 read_text(reader, StructuralLimits::kMaxProvenanceDetailBytes,
                           "provenance.detail", true));
  if (detail.size() > limits.max_provenance_detail_bytes) {
    return Error(ErrorCode::store_limit_exceeded,
                 "provenance.detail in the payload exceeds the configured limit",
                 "provenance.detail");
  }

  auto record = ProvenanceRecord::make(source, std::move(principal), sequence, generation,
                                       timestamp.value(), std::move(epoch), std::move(reason),
                                       std::move(detail));
  if (!record.has_value()) {
    return Error(ErrorCode::store_corrupt,
                 "provenance in the payload is invalid: " + record.error().message(),
                 "provenance");
  }
  return std::move(record).value();
}

[[nodiscard]] Result<DataCenterRecord> read_record(CanonicalReader& reader,
                                                   const RegistryLimits& limits) {
  DCR_TRY_ASSIGN(DataCenterId id, read_token<DataCenterId>(reader, "record.id"));
  DCR_TRY_ASSIGN(const MetadataRevision revision, read_revision(reader, "record.revision"));
  DCR_TRY_ASSIGN(std::string display_name,
                 read_text(reader, StructuralLimits::kMaxDisplayNameBytes, "record.display_name",
                           false));

  DCR_TRY_ASSIGN(std::size_t alias_count,
                 reader.count("record.aliases", StructuralLimits::kMaxAliasesPerRecord));
  std::vector<Alias> aliases;
  aliases.reserve(alias_count);
  for (std::size_t index = 0; index < alias_count; ++index) {
    aliases.push_back(std::move(read_token<Alias>(reader, "record.aliases").value()));
  }

  DCR_TRY_ASSIGN(std::size_t membership_count,
                 reader.count("record.memberships", StructuralLimits::kMaxMembershipsPerRecord));
  std::vector<SiteMembership> memberships;
  memberships.reserve(membership_count);
  for (std::size_t index = 0; index < membership_count; ++index) {
    DCR_TRY_ASSIGN(SiteId site, read_token<SiteId>(reader, "record.memberships.site"));
    DCR_TRY_ASSIGN(const MembershipRole role,
                   read_enum<MembershipRole>(reader, 1, 4, "record.memberships.role"));
    memberships.push_back(SiteMembership{std::move(site), role});
  }

  DCR_TRY_ASSIGN(FacilityMetadata facility, read_facility(reader, limits));
  DCR_TRY_ASSIGN(MetadataMap extensions, read_metadata_map(reader, limits, "record.extensions"));

  DCR_TRY_ASSIGN(const OwnershipKind kind,
                 read_enum<OwnershipKind>(reader, 1, 5, "record.ownership.kind"));
  DCR_TRY_ASSIGN(const bool has_scope, reader.boolean());
  OwnershipScope ownership = OwnershipScope::unassigned();
  if (has_scope) {
    DCR_TRY_ASSIGN(OwnershipScopeId scope,
                   read_token<OwnershipScopeId>(reader, "record.ownership.scope"));
    auto made = OwnershipScope::make(kind, std::move(scope));
    if (!made.has_value()) {
      return Error(ErrorCode::store_corrupt,
                   "record.ownership in the payload is invalid: " + made.error().message(),
                   "record.ownership");
    }
    ownership = std::move(made).value();
  } else if (kind != OwnershipKind::unassigned) {
    return corrupt("record.ownership has kind '" + std::string(to_string(kind)) +
                       "' but no scope identifier",
                   "record.ownership");
  }

  DCR_TRY_ASSIGN(const std::uint16_t model_major, reader.u16());
  DCR_TRY_ASSIGN(const std::uint16_t model_minor, reader.u16());
  DCR_TRY_ASSIGN(const std::uint64_t capability_mask, reader.u64());
  auto compatibility = CompatibilityKey::make(model_major, model_minor, capability_mask);
  if (!compatibility.has_value()) {
    return Error(ErrorCode::store_corrupt,
                 "record.compatibility in the payload is invalid: " +
                     compatibility.error().message(),
                 "record.compatibility");
  }

  DCR_TRY_ASSIGN(const LifecycleState state,
                 read_enum<LifecycleState>(reader, 1, 7, "record.state"));
  DCR_TRY_ASSIGN(const RegistryGeneration created_generation,
                 read_generation(reader, "record.created_generation"));

  std::optional<RegistryGeneration> retired_generation;
  DCR_TRY_ASSIGN(const bool has_retired, reader.boolean());
  if (has_retired) {
    retired_generation = read_generation(reader, "record.retired_generation").value();
  }

  std::optional<RetirementInfo> retirement;
  DCR_TRY_ASSIGN(const bool has_retirement, reader.boolean());
  if (has_retirement) {
    std::optional<ReasonCode> reason;
    DCR_TRY_ASSIGN(const bool has_reason, reader.boolean());
    if (has_reason) {
      reason = read_token<ReasonCode>(reader, "record.retirement.reason").value();
    }
    DCR_TRY_ASSIGN(const RegistryGeneration retirement_generation,
                   read_generation(reader, "record.retirement.generation"));
    std::optional<DataCenterId> replaced_by;
    DCR_TRY_ASSIGN(const bool has_replaced_by, reader.boolean());
    if (has_replaced_by) {
      replaced_by =
          read_token<DataCenterId>(reader, "record.retirement.replaced_by").value();
    }
    retirement = RetirementInfo{std::move(reason), retirement_generation,
                                std::move(replaced_by)};
  }

  std::optional<DataCenterId> replaces;
  DCR_TRY_ASSIGN(const bool has_replaces, reader.boolean());
  if (has_replaces) {
    replaces = read_token<DataCenterId>(reader, "record.replaces").value();
  }

  DCR_TRY_ASSIGN(ProvenanceRecord last_provenance, read_provenance(reader, limits));
  DCR_TRY_ASSIGN(const SequenceNumber last_sequence, read_sequence(reader, "record.last_sequence"));

  DataCenterRecord record{std::move(id),           revision,
                          std::move(display_name), std::move(aliases),
                          std::move(memberships),  std::move(facility),
                          std::move(extensions),   std::move(ownership),
                          compatibility.value(),   state,
                          created_generation,      retired_generation,
                          std::move(retirement),   std::move(replaces),
                          std::move(last_provenance),
                          last_sequence};

  if (auto result = validate_record(record, limits); !result.has_value()) {
    return wrap_payload_error(result.error(),
                              "record '" + record.id.value() + "' in the payload is invalid");
  }
  return record;
}

[[nodiscard]] Result<HistoryEntry> read_history(CanonicalReader& reader,
                                                const RegistryLimits& limits) {
  DCR_TRY_ASSIGN(const SequenceNumber sequence, read_sequence(reader, "history.sequence"));
  DCR_TRY_ASSIGN(const RegistryGeneration generation,
                 read_generation(reader, "history.generation"));
  DCR_TRY_ASSIGN(DataCenterId id, read_token<DataCenterId>(reader, "history.id"));
  DCR_TRY_ASSIGN(const HistoryAction action,
                 read_enum<HistoryAction>(reader, 1, 8, "history.action"));
  DCR_TRY_ASSIGN(const MetadataRevision revision, read_revision(reader, "history.revision"));
  std::optional<LifecycleState> previous_state;
  DCR_TRY_ASSIGN(const bool has_previous, reader.boolean());
  if (has_previous) {
    previous_state = read_enum<LifecycleState>(reader, 1, 7, "history.previous_state").value();
  }
  DCR_TRY_ASSIGN(const LifecycleState new_state,
                 read_enum<LifecycleState>(reader, 1, 7, "history.new_state"));
  DCR_TRY_ASSIGN(ProvenanceRecord provenance, read_provenance(reader, limits));

  if (sequence.value() != provenance.sequence().value() ||
      generation.value() != provenance.generation().value()) {
    return corrupt("a history entry and its provenance disagree about sequence or generation",
                   "history");
  }
  return HistoryEntry{sequence,      generation,   std::move(id), action,
                      revision,      previous_state, new_state,     std::move(provenance)};
}

[[nodiscard]] Result<IdempotencyEntry> read_idempotency(CanonicalReader& reader) {
  DCR_TRY_ASSIGN(IdempotencyKey key, read_token<IdempotencyKey>(reader, "idempotency.key"));
  DCR_TRY_ASSIGN(const std::string_view digest_bytes, reader.raw());
  if (digest_bytes.size() != Sha256Digest::kSize) {
    return corrupt("an idempotency command digest must be 32 bytes", "idempotency.digest");
  }
  Sha256Digest::Bytes digest_raw{};
  std::copy(digest_bytes.begin(), digest_bytes.end(), digest_raw.begin());

  DCR_TRY_ASSIGN(const MutationStatus status,
                 read_enum<MutationStatus>(reader, 1, 4, "idempotency.status"));
  DCR_TRY_ASSIGN(DataCenterId id, read_token<DataCenterId>(reader, "idempotency.id"));
  DCR_TRY_ASSIGN(const RegistryGeneration generation,
                 read_generation(reader, "idempotency.generation"));
  DCR_TRY_ASSIGN(const MetadataRevision revision, read_revision(reader, "idempotency.revision"));
  std::optional<DataCenterId> related_id;
  DCR_TRY_ASSIGN(const bool has_related, reader.boolean());
  if (has_related) {
    related_id = read_token<DataCenterId>(reader, "idempotency.related_id").value();
  }
  std::optional<SequenceNumber> sequence;
  DCR_TRY_ASSIGN(const bool has_sequence, reader.boolean());
  if (has_sequence) {
    sequence = read_sequence(reader, "idempotency.sequence").value();
  }
  DCR_TRY_ASSIGN(const SequenceNumber recorded_sequence,
                 read_sequence(reader, "idempotency.recorded_sequence"));

  return IdempotencyEntry{std::move(key), Sha256Digest(digest_raw), status,
                          std::move(id),   generation,             revision,
                          std::move(related_id), std::move(sequence), recorded_sequence};
}

}  // namespace

RegistryLimits tighten_limits(const RegistryLimits& left, const RegistryLimits& right) noexcept {
  RegistryLimits tightened;
  tightened.max_records = std::min(left.max_records, right.max_records);
  tightened.max_query_results = std::min(left.max_query_results, right.max_query_results);
  tightened.max_aliases_per_record = std::min(left.max_aliases_per_record, right.max_aliases_per_record);
  tightened.max_memberships_per_record =
      std::min(left.max_memberships_per_record, right.max_memberships_per_record);
  tightened.max_metadata_entries = std::min(left.max_metadata_entries, right.max_metadata_entries);
  tightened.max_metadata_value_bytes =
      std::min(left.max_metadata_value_bytes, right.max_metadata_value_bytes);
  tightened.max_display_name_bytes =
      std::min(left.max_display_name_bytes, right.max_display_name_bytes);
  tightened.max_address_lines = std::min(left.max_address_lines, right.max_address_lines);
  tightened.max_address_line_bytes =
      std::min(left.max_address_line_bytes, right.max_address_line_bytes);
  tightened.max_provenance_detail_bytes =
      std::min(left.max_provenance_detail_bytes, right.max_provenance_detail_bytes);
  tightened.max_history_entries = std::min(left.max_history_entries, right.max_history_entries);
  tightened.max_idempotency_entries =
      std::min(left.max_idempotency_entries, right.max_idempotency_entries);
  tightened.max_snapshot_bytes = std::min(left.max_snapshot_bytes, right.max_snapshot_bytes);

  tightened.max_records = std::min(tightened.max_records, StructuralLimits::kMaxRecords);
  tightened.max_aliases_per_record =
      std::min(tightened.max_aliases_per_record, StructuralLimits::kMaxAliasesPerRecord);
  tightened.max_memberships_per_record =
      std::min(tightened.max_memberships_per_record, StructuralLimits::kMaxMembershipsPerRecord);
  tightened.max_metadata_entries =
      std::min(tightened.max_metadata_entries, StructuralLimits::kMaxMetadataEntries);
  tightened.max_metadata_value_bytes =
      std::min(tightened.max_metadata_value_bytes, StructuralLimits::kMaxMetadataValueBytes);
  tightened.max_display_name_bytes =
      std::min(tightened.max_display_name_bytes, StructuralLimits::kMaxDisplayNameBytes);
  tightened.max_address_lines =
      std::min(tightened.max_address_lines, StructuralLimits::kMaxAddressLines);
  tightened.max_address_line_bytes =
      std::min(tightened.max_address_line_bytes, StructuralLimits::kMaxAddressLineBytes);
  tightened.max_provenance_detail_bytes = std::min(tightened.max_provenance_detail_bytes,
                                                   StructuralLimits::kMaxProvenanceDetailBytes);
  tightened.max_snapshot_bytes =
      std::min(tightened.max_snapshot_bytes, StructuralLimits::kMaxSnapshotBytes);
  return tightened;
}

Result<std::string> encode_state(const RegistryState& state) {
  if (auto result = validate_state(state); !result.has_value()) {
    return result.error();
  }
  CanonicalWriter writer;
  writer.u16(state.format_major);
  writer.u16(state.format_minor);
  writer.varint(state.generation.value());
  writer.boolean(state.external_epoch.has_value());
  if (state.external_epoch.has_value()) {
    writer.varint(state.external_epoch->value());
    writer.text(state.external_epoch->issuer().value());
  }
  writer.varint(state.last_sequence.value());
  writer.varint(state.history_dropped);
  write_limits(writer, state.limits);

  writer.varint(state.records.size());
  for (const auto& record : state.records) {
    write_record(writer, record);
  }

  writer.varint(state.history.size());
  for (const auto& entry : state.history) {
    write_history(writer, entry);
  }

  writer.varint(state.idempotency.size());
  for (const auto& entry : state.idempotency) {
    write_idempotency(writer, entry);
  }

  if (writer.size() > state.limits.max_snapshot_bytes) {
    return Error(ErrorCode::limit_exceeded,
                 "the canonical payload would be " + std::to_string(writer.size()) +
                     " bytes, which exceeds max_snapshot_bytes (" +
                     std::to_string(state.limits.max_snapshot_bytes) + ")",
                 "max_snapshot_bytes");
  }
  return writer.take();
}

Result<RegistryState> decode_state(std::string_view payload,
                                   const RegistryLimits& configured_limits) {
  if (auto result = configured_limits.validate(); !result.has_value()) {
    return result.error();
  }
  CanonicalReader reader(payload);

  RegistryState state = RegistryState::empty(configured_limits);

  DCR_TRY_ASSIGN(state.format_major, reader.u16());
  DCR_TRY_ASSIGN(state.format_minor, reader.u16());
  if (state.format_major != kSnapshotFormatMajor) {
    return Error(ErrorCode::store_incompatible_version,
                 "the payload declares snapshot format major " +
                     std::to_string(state.format_major) + ", but this build writes and reads " +
                     std::to_string(kSnapshotFormatMajor),
                 "format_major");
  }
  if (state.format_minor > kSnapshotFormatMinor) {
    return Error(ErrorCode::store_incompatible_version,
                 "the payload declares snapshot format minor " +
                     std::to_string(state.format_minor) + ", which is newer than " +
                     std::to_string(kSnapshotFormatMinor),
                 "format_minor");
  }

  DCR_TRY_ASSIGN(state.generation, read_generation(reader, "registry_generation"));

  DCR_TRY_ASSIGN(const bool has_epoch, reader.boolean());
  if (has_epoch) {
    DCR_TRY_ASSIGN(const std::uint64_t value, reader.varint());
    DCR_TRY_ASSIGN(EpochIssuerId issuer, read_token<EpochIssuerId>(reader, "external_epoch"));
    state.external_epoch = EpochToken::make(std::move(issuer), value).value();
  }

  DCR_TRY_ASSIGN(state.last_sequence, read_sequence(reader, "last_sequence"));
  DCR_TRY_ASSIGN(state.history_dropped, reader.varint());

  RegistryLimits persisted;
  DCR_TRY_ASSIGN(persisted.max_records, reader.varint());
  DCR_TRY_ASSIGN(persisted.max_query_results, reader.varint());
  DCR_TRY_ASSIGN(persisted.max_aliases_per_record, read_u32_varint(reader, "limits.max_aliases_per_record"));
  DCR_TRY_ASSIGN(persisted.max_memberships_per_record, read_u32_varint(reader, "limits.max_memberships_per_record"));
  DCR_TRY_ASSIGN(persisted.max_metadata_entries, read_u32_varint(reader, "limits.max_metadata_entries"));
  DCR_TRY_ASSIGN(persisted.max_metadata_value_bytes, read_u32_varint(reader, "limits.max_metadata_value_bytes"));
  DCR_TRY_ASSIGN(persisted.max_display_name_bytes, read_u32_varint(reader, "limits.max_display_name_bytes"));
  DCR_TRY_ASSIGN(persisted.max_address_lines, read_u32_varint(reader, "limits.max_address_lines"));
  DCR_TRY_ASSIGN(persisted.max_address_line_bytes, read_u32_varint(reader, "limits.max_address_line_bytes"));
  DCR_TRY_ASSIGN(persisted.max_provenance_detail_bytes, read_u32_varint(reader, "limits.max_provenance_detail_bytes"));
  DCR_TRY_ASSIGN(persisted.max_history_entries, read_u32_varint(reader, "limits.max_history_entries"));
  DCR_TRY_ASSIGN(persisted.max_idempotency_entries, read_u32_varint(reader, "limits.max_idempotency_entries"));
  DCR_TRY_ASSIGN(persisted.max_snapshot_bytes, reader.varint());

  // The persisted limits are only ever allowed to tighten, never to widen.
  const RegistryLimits effective = tighten_limits(persisted, configured_limits);


  DCR_TRY_ASSIGN(const std::size_t record_count,
                 reader.count("record table", static_cast<std::size_t>(effective.max_records)));
  state.records.reserve(record_count);
  for (std::size_t index = 0; index < record_count; ++index) {
    DCR_TRY_ASSIGN(DataCenterRecord record, read_record(reader, effective));
    if (index > 0 && !(state.records[index - 1].id < record.id)) {
      return corrupt("records in the payload are not in strictly ascending canonical order",
                     "records");
    }
    state.records.push_back(std::move(record));
  }

  DCR_TRY_ASSIGN(const std::size_t history_count,
                 reader.count("history window",
                              static_cast<std::size_t>(effective.max_history_entries)));
  state.history.reserve(history_count);
  for (std::size_t index = 0; index < history_count; ++index) {
    DCR_TRY_ASSIGN(HistoryEntry entry, read_history(reader, effective));
    if (index > 0 && !(state.history[index - 1].sequence < entry.sequence)) {
      return corrupt("history entries in the payload are not in strictly ascending sequence order",
                     "history");
    }
    state.history.push_back(std::move(entry));
  }

  DCR_TRY_ASSIGN(const std::size_t idempotency_count,
                 reader.count("idempotency table",
                              static_cast<std::size_t>(effective.max_idempotency_entries)));
  state.idempotency.reserve(idempotency_count);
  for (std::size_t index = 0; index < idempotency_count; ++index) {
    DCR_TRY_ASSIGN(IdempotencyEntry entry, read_idempotency(reader));
    if (index > 0 && !(state.idempotency[index - 1].key < entry.key)) {
      return corrupt("idempotency entries in the payload are not in strictly ascending key order",
                     "idempotency");
    }
    state.idempotency.push_back(std::move(entry));
  }

  DCR_TRY(reader.expect_end());

  // The state now runs under the configured limits; validation uses them.
  state.limits = configured_limits;
  if (auto result = validate_state(state); !result.has_value()) {
    return wrap_payload_error(result.error(),
                              "the decoded payload violates a registry invariant");
  }
  return state;
}

}  // namespace dcr::internal
