// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "dcr/identity.hpp"

#include <algorithm>
#include <array>
#include <string_view>

#include "internal/text.hpp"

namespace dcr {

template <class Policy>
Result<StrongToken<Policy>> StrongToken<Policy>::parse(std::string_view text) {
  DCR_TRY_ASSIGN(std::string canonical, internal::validate_token(Policy::value, text));
  return StrongToken<Policy>(std::move(canonical));
}

// The identifier families are a closed set: the library instantiates exactly
// these, and no consumer can add one without editing this list.
template class StrongToken<DataCenterIdPolicy>;
template class StrongToken<SiteIdPolicy>;
template class StrongToken<AliasPolicy>;
template class StrongToken<PrincipalIdPolicy>;
template class StrongToken<OwnershipScopeIdPolicy>;
template class StrongToken<EpochIssuerIdPolicy>;
template class StrongToken<ReasonCodePolicy>;
template class StrongToken<IdempotencyKeyPolicy>;
template class StrongToken<RegionCodePolicy>;
template class StrongToken<MetroCodePolicy>;
template class StrongToken<MetadataKeyPolicy>;

namespace {

/// ISO 3166-1 alpha-2 assigned codes, sorted so that membership can be tested
/// with a binary search.
constexpr std::array<std::string_view, 249> kIso3166Alpha2 = {
    "AD", "AE", "AF", "AG", "AI", "AL", "AM", "AO", "AQ", "AR", "AS", "AT", "AU", "AW", "AX",
    "AZ", "BA", "BB", "BD", "BE", "BF", "BG", "BH", "BI", "BJ", "BL", "BM", "BN", "BO", "BQ",
    "BR", "BS", "BT", "BV", "BW", "BY", "BZ", "CA", "CC", "CD", "CF", "CG", "CH", "CI", "CK",
    "CL", "CM", "CN", "CO", "CR", "CU", "CV", "CW", "CX", "CY", "CZ", "DE", "DJ", "DK", "DM",
    "DO", "DZ", "EC", "EE", "EG", "EH", "ER", "ES", "ET", "FI", "FJ", "FK", "FM", "FO", "FR",
    "GA", "GB", "GD", "GE", "GF", "GG", "GH", "GI", "GL", "GM", "GN", "GP", "GQ", "GR", "GS",
    "GT", "GU", "GW", "GY", "HK", "HM", "HN", "HR", "HT", "HU", "ID", "IE", "IL", "IM", "IN",
    "IO", "IQ", "IR", "IS", "IT", "JE", "JM", "JO", "JP", "KE", "KG", "KH", "KI", "KM", "KN",
    "KP", "KR", "KW", "KY", "KZ", "LA", "LB", "LC", "LI", "LK", "LR", "LS", "LT", "LU", "LV",
    "LY", "MA", "MC", "MD", "ME", "MF", "MG", "MH", "MK", "ML", "MM", "MN", "MO", "MP", "MQ",
    "MR", "MS", "MT", "MU", "MV", "MW", "MX", "MY", "MZ", "NA", "NC", "NE", "NF", "NG", "NI",
    "NL", "NO", "NP", "NR", "NU", "NZ", "OM", "PA", "PE", "PF", "PG", "PH", "PK", "PL", "PM",
    "PN", "PR", "PS", "PT", "PW", "PY", "QA", "RE", "RO", "RS", "RU", "RW", "SA", "SB", "SC",
    "SD", "SE", "SG", "SH", "SI", "SJ", "SK", "SL", "SM", "SN", "SO", "SR", "SS", "ST", "SV",
    "SX", "SY", "SZ", "TC", "TD", "TF", "TG", "TH", "TJ", "TK", "TL", "TM", "TN", "TO", "TR",
    "TT", "TV", "TW", "TZ", "UA", "UG", "UM", "US", "UY", "UZ", "VA", "VC", "VE", "VG", "VI",
    "VN", "VU", "WF", "WS", "YE", "YT", "ZA", "ZM", "ZW"};

}  // namespace

bool CountryCode::is_assigned(std::string_view text) noexcept {
  return std::binary_search(kIso3166Alpha2.begin(), kIso3166Alpha2.end(), text);
}

Result<CountryCode> CountryCode::parse(std::string_view text) {
  if (text.size() != 2) {
    return Error(ErrorCode::invalid_metadata,
                 "a country code must be exactly two characters, got " +
                     std::to_string(text.size()),
                 "CountryCode");
  }
  for (const char value : text) {
    if (value < 'A' || value > 'Z') {
      return Error(ErrorCode::invalid_metadata,
                   "a country code must be two upper-case ASCII letters, got '" +
                       std::string(text) + "'",
                   "CountryCode");
    }
  }
  if (!is_assigned(text)) {
    return Error(ErrorCode::invalid_metadata,
                 "'" + std::string(text) + "' is not an assigned ISO 3166-1 alpha-2 country code",
                 "CountryCode");
  }
  return CountryCode(std::string(text));
}

}  // namespace dcr
