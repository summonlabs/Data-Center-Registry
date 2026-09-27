// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Strongly typed identifiers.
//
// The registry distinguishes identifier families by type, not by convention.
// Two identifier families are never implicitly convertible, never comparable
// and never interchangeable as container keys, so a SiteId cannot be passed
// where a DataCenterId is expected even though both are short ASCII strings.
//
// Every family has one canonical textual form. Parsing is strict: input is
// either already canonical or it is rejected. Nothing is case-folded, trimmed
// or otherwise normalised into authoritative state, because silent
// normalisation would let two spellings claim the same identity.
//
// Ordering for every family is the byte-wise lexicographic order of the
// canonical text. This is the order used by every index and by canonical
// serialisation, so enumeration order is defined by the type, not by the
// container that happens to hold it.

#ifndef DCR_IDENTITY_HPP
#define DCR_IDENTITY_HPP

#include <compare>
#include <cstddef>
#include <cstdint>
#include <ostream>
#include <string>
#include <string_view>

#include "dcr/result.hpp"

namespace dcr {

/// The shape rule applied to the part of a token that follows its prefix.
enum class TokenSyntax : std::uint8_t {
  /// `[a-z0-9]` at both ends, `[a-z0-9-]` between, no doubled hyphen.
  slug,
  /// A prefixed slug, e.g. `dc-ashburn-01`.
  prefixed_slug,
  /// `[a-z][a-z0-9_]*`: a machine-readable snake_case code.
  snake_case,
  /// `[A-Za-z0-9._:-]*` starting with an alphanumeric: an opaque retry or
  /// correlation token whose content the registry never interprets.
  opaque,
  /// `[a-z][a-z0-9]*` segments joined by single dots, e.g. `power.feed.a`.
  dotted,
  /// A principal reference: `[a-z0-9]` at both ends with
  /// `[a-z0-9._:@/+~-]` between. Intended for operator, service and
  /// controller identities supplied by an external authority.
  principal,
};

/// Describes one identifier family: its name for diagnostics, its required
/// prefix, its syntax rule and its canonical length bounds (prefix included).
struct TokenPolicy {
  const char* type_name;
  const char* prefix;
  TokenSyntax syntax;
  std::size_t min_length;
  std::size_t max_length;
};

/// Canonical identity of a data center. Stable for the life of the record;
/// ordinary metadata changes never change it. A genuinely new facility is a
/// new DataCenterId, optionally linked to its predecessor through the
/// replacement operation.
struct DataCenterIdPolicy {
  static constexpr TokenPolicy value{"DataCenterId", "dc-", TokenSyntax::prefixed_slug, 6, 64};
};

/// Reference to a site owned by the site control plane. The registry records
/// membership in a site; it does not own the site object.
struct SiteIdPolicy {
  static constexpr TokenPolicy value{"SiteId", "site-", TokenSyntax::prefixed_slug, 8, 72};
};

/// A secondary lookup key for a data center. Aliases exist so that historical
/// or human-facing names keep resolving; they are never identity.
struct AliasPolicy {
  static constexpr TokenPolicy value{"Alias", "", TokenSyntax::slug, 3, 64};
};

/// An actor: an operator, a service, or a controller that a mutation is
/// attributed to.
struct PrincipalIdPolicy {
  static constexpr TokenPolicy value{"PrincipalId", "", TokenSyntax::principal, 3, 128};
};

/// The administrative scope that owns a record. Scope semantics belong to the
/// tenancy layer; the registry only records which scope a record belongs to
/// and never evaluates policy over it.
struct OwnershipScopeIdPolicy {
  static constexpr TokenPolicy value{"OwnershipScopeId", "scope-", TokenSyntax::prefixed_slug, 9,
                                     72};
};

/// Identifies the external authority that issued a control-plane epoch token.
/// Epoch election belongs to the control-plane-epoch system; the registry only
/// records tokens that such an authority hands it.
struct EpochIssuerIdPolicy {
  static constexpr TokenPolicy value{"EpochIssuerId", "cpe-", TokenSyntax::prefixed_slug, 7, 72};
};

/// A machine-readable reason code attached to a lifecycle transition, a
/// retirement or an error surfaced by a tool.
struct ReasonCodePolicy {
  static constexpr TokenPolicy value{"ReasonCode", "", TokenSyntax::snake_case, 1, 48};
};

/// An opaque caller-supplied token used to make a retryable mutation
/// idempotent. The registry stores a digest of the command alongside it and
/// never interprets the key itself.
struct IdempotencyKeyPolicy {
  static constexpr TokenPolicy value{"IdempotencyKey", "", TokenSyntax::opaque, 8, 128};
};

/// A caller-supplied administrative region label.
struct RegionCodePolicy {
  static constexpr TokenPolicy value{"RegionCode", "", TokenSyntax::slug, 1, 32};
};

/// A caller-supplied metropolitan-area label.
struct MetroCodePolicy {
  static constexpr TokenPolicy value{"MetroCode", "", TokenSyntax::slug, 1, 32};
};

/// A namespaced key in deterministic extension metadata. Keys beginning with
/// `dcr.` are reserved to the registry and are rejected from caller input so
/// that registry-reserved metadata cannot be spoofed.
struct MetadataKeyPolicy {
  static constexpr TokenPolicy value{"MetadataKey", "", TokenSyntax::dotted, 1, 96};
};

/// No implicit conversions exist between identifier families. A
/// StrongToken<Policy> can only be produced by parsing canonical text, so a
/// value of this type is always syntactically valid.
template <class Policy>
class StrongToken {
 public:
  StrongToken() = delete;
  StrongToken(const StrongToken&) = default;
  StrongToken(StrongToken&&) noexcept = default;
  StrongToken& operator=(const StrongToken&) = default;
  StrongToken& operator=(StrongToken&&) noexcept = default;
  ~StrongToken() = default;

  /// Parses canonical text. Rejects anything that is not already canonical.
  [[nodiscard]] static Result<StrongToken> parse(std::string_view text);

  /// The canonical text.
  [[nodiscard]] const std::string& value() const noexcept { return value_; }
  [[nodiscard]] const char* c_str() const noexcept { return value_.c_str(); }

  /// The family name, for diagnostics: "DataCenterId", "SiteId", ...
  [[nodiscard]] static constexpr const char* type_name() noexcept {
    return Policy::value.type_name;
  }

  /// The canonical text.
  [[nodiscard]] std::string to_string() const { return value_; }

  friend bool operator==(const StrongToken& left, const StrongToken& right) noexcept {
    return left.value_ == right.value_;
  }
  friend bool operator!=(const StrongToken& left, const StrongToken& right) noexcept {
    return !(left == right);
  }
  friend std::strong_ordering operator<=>(const StrongToken& left,
                                          const StrongToken& right) noexcept {
    return left.value_ <=> right.value_;
  }
  friend std::ostream& operator<<(std::ostream& out, const StrongToken& token) {
    return out << token.value_;
  }

 private:
  explicit StrongToken(std::string canonical) : value_(std::move(canonical)) {}

  std::string value_;
};

// The identifier families are a closed set. The library instantiates exactly
// the following types; a consumer cannot mint a new family without adding a
// policy and rebuilding the library, which is deliberate: an identifier family
// is a trust boundary, not a convenience.
using DataCenterId = StrongToken<DataCenterIdPolicy>;
using SiteId = StrongToken<SiteIdPolicy>;
using Alias = StrongToken<AliasPolicy>;
using PrincipalId = StrongToken<PrincipalIdPolicy>;
using OwnershipScopeId = StrongToken<OwnershipScopeIdPolicy>;
using EpochIssuerId = StrongToken<EpochIssuerIdPolicy>;
using ReasonCode = StrongToken<ReasonCodePolicy>;
using IdempotencyKey = StrongToken<IdempotencyKeyPolicy>;
using RegionCode = StrongToken<RegionCodePolicy>;
using MetroCode = StrongToken<MetroCodePolicy>;
using MetadataKey = StrongToken<MetadataKeyPolicy>;

/// An ISO 3166-1 alpha-2 country code. The assigned-code set is validated, so
/// unreserved two-letter combinations are rejected rather than stored.
class CountryCode {
 public:
  CountryCode() = delete;
  CountryCode(const CountryCode&) = default;
  CountryCode(CountryCode&&) noexcept = default;
  CountryCode& operator=(const CountryCode&) = default;
  CountryCode& operator=(CountryCode&&) noexcept = default;
  ~CountryCode() = default;

  [[nodiscard]] static Result<CountryCode> parse(std::string_view text);

  /// True when the two-letter combination is an assigned ISO 3166-1 alpha-2
  /// code.
  [[nodiscard]] static bool is_assigned(std::string_view text) noexcept;

  [[nodiscard]] const std::string& value() const noexcept { return value_; }
  [[nodiscard]] const char* c_str() const noexcept { return value_.c_str(); }
  [[nodiscard]] std::string to_string() const { return value_; }

  friend bool operator==(const CountryCode& left, const CountryCode& right) noexcept {
    return left.value_ == right.value_;
  }
  friend bool operator!=(const CountryCode& left, const CountryCode& right) noexcept {
    return !(left == right);
  }
  friend std::strong_ordering operator<=>(const CountryCode& left,
                                          const CountryCode& right) noexcept {
    return left.value_ <=> right.value_;
  }

 private:
  explicit CountryCode(std::string canonical) : value_(std::move(canonical)) {}
  std::string value_;
};

}  // namespace dcr

#endif  // DCR_IDENTITY_HPP
