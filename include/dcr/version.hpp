// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Version and build identity of the Data Center Registry library.

#ifndef DCR_VERSION_HPP
#define DCR_VERSION_HPP

#include <cstdint>
#include <string_view>

namespace dcr {

/// Major version of the public API. A change here is a source-incompatible
/// change to the public headers.
inline constexpr std::uint32_t kApiVersionMajor = 1;

/// Minor version of the public API. Additive changes only.
inline constexpr std::uint32_t kApiVersionMinor = 0;

/// Patch version of the library implementation.
inline constexpr std::uint32_t kApiVersionPatch = 0;

/// Version of the library, as a dotted string.
inline constexpr std::string_view kVersionString = "1.0.0";

/// Version of the on-disk snapshot payload produced and accepted by this
/// library. It is independent of the API version: a payload format change is a
/// store-compatibility change and is reported by the persistence layer, never
/// silently accepted.
inline constexpr std::uint16_t kSnapshotFormatMajor = 1;
inline constexpr std::uint16_t kSnapshotFormatMinor = 0;

/// Identifies the software that produced a snapshot, for diagnostics only.
/// It never participates in acceptance decisions.
inline constexpr std::string_view kProducerName = "data-center-registry";

}  // namespace dcr

#endif  // DCR_VERSION_HPP
