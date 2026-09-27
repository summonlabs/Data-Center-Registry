// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "dcr/generation.hpp"

#include <limits>

#include "internal/checked.hpp"
#include "internal/text.hpp"

namespace dcr {

template <class Tag>
Result<Counter<Tag>> Counter<Tag>::from_value(Value value) {
  if (value < kMinValue) {
    return Error(ErrorCode::invalid_argument,
                 std::string(Tag::kTypeName) + " cannot be below " + std::to_string(kMinValue),
                 std::string(Tag::kTypeName));
  }
  return Counter<Tag>(value);
}

template <class Tag>
Result<Counter<Tag>> Counter<Tag>::successor() const {
  if (value_ == kMaxValue) {
    return Error(ErrorCode::limit_exceeded,
                 std::string(Tag::kTypeName) + " is exhausted at " + std::to_string(kMaxValue),
                 std::string(Tag::kTypeName));
  }
  return Counter<Tag>(value_ + 1U);
}

template <class Tag>
Result<Counter<Tag>> Counter<Tag>::parse(std::string_view text) {
  DCR_TRY_ASSIGN(const std::uint64_t value, internal::parse_u64(text, Tag::kTypeName));
  return from_value(value);
}

template class Counter<RegistryGenerationTag>;
template class Counter<MetadataRevisionTag>;
template class Counter<SequenceNumberTag>;

Result<EpochToken> EpochToken::make(EpochIssuerId issuer_id, std::uint64_t value) {
  return EpochToken(std::move(issuer_id), value);
}

std::optional<std::strong_ordering> EpochToken::compare(const EpochToken& other) const noexcept {
  if (!(issuer_ == other.issuer_)) {
    return std::nullopt;
  }
  return value_ <=> other.value_;
}

std::string EpochToken::to_string() const {
  return issuer_.value() + "@" + std::to_string(value_);
}

Result<EpochToken> EpochToken::parse(std::string_view text) {
  const std::size_t separator = text.rfind('@');
  if (separator == std::string_view::npos || separator == 0 || separator + 1 >= text.size()) {
    return Error(ErrorCode::invalid_argument,
                 "an epoch token must have the form <issuer>@<value>, got '" + std::string(text) +
                     "'",
                 "EpochToken");
  }
  DCR_TRY_ASSIGN(EpochIssuerId issuer, EpochIssuerId::parse(text.substr(0, separator)));
  DCR_TRY_ASSIGN(const std::uint64_t value,
                 internal::parse_u64(text.substr(separator + 1), "EpochToken.value"));
  return EpochToken(std::move(issuer), value);
}

}  // namespace dcr
