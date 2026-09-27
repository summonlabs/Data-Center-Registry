// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Site membership.
//
// A data center belongs to a site. The site object itself is owned by the site
// control plane; this library stores a typed reference to it and the role the
// data center plays there. Membership is a set keyed by SiteId, ordered by
// SiteId, with at most one `primary` member per record.

#ifndef DCR_MEMBERSHIP_HPP
#define DCR_MEMBERSHIP_HPP

#include <cstdint>
#include <string_view>

#include "dcr/identity.hpp"
#include "dcr/result.hpp"

namespace dcr {

enum class MembershipRole : std::uint8_t {
  /// The site this data center is primarily registered under. A live record
  /// has exactly one primary membership.
  primary = 1,
  /// An additional site the data center participates in.
  secondary = 2,
  /// A site traversed or shared but not occupied.
  transit = 3,
  /// A site held as a declared standby for this data center.
  standby = 4,
};

[[nodiscard]] std::string_view to_string(MembershipRole role) noexcept;
[[nodiscard]] Result<MembershipRole> parse_membership_role(std::string_view token);

/// One membership edge. Both fields are validated types, so any constructed
/// value is well formed; the set-level rules (one entry per site, at most one
/// primary, exactly one primary while live) are enforced by the record and by
/// the mutation engine.
struct SiteMembership {
  SiteId site;
  MembershipRole role = MembershipRole::primary;

  friend bool operator==(const SiteMembership& left, const SiteMembership& right) {
    return left.site == right.site && left.role == right.role;
  }
  friend bool operator!=(const SiteMembership& left, const SiteMembership& right) {
    return !(left == right);
  }
  friend bool operator<(const SiteMembership& left, const SiteMembership& right) {
    return left.site < right.site;
  }
};

}  // namespace dcr

#endif  // DCR_MEMBERSHIP_HPP
