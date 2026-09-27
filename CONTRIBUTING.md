# Contributing to Data Center Registry

We welcome contributions from individuals and organizations.

## Contribution terms

Contributions are submitted under the terms of the Apache License 2.0. No
Contributor License Agreement (CLA) is required. By submitting a contribution
you agree that it may be distributed under the Apache License 2.0.

## Scope and boundaries

Keep changes within the repository's architectural and system boundary. Data
Center Registry owns canonical identity and authoritative metadata for data
centers as facility control-plane objects: stable identities, site membership,
control-plane generations, lifecycle state, ownership attribution, provenance
and deterministic extension metadata.

It does not own the physical site object, facility topology, the asset registry,
the control-plane epoch, capacity planning, placement, reservations, power or
cooling actuation, maintenance orchestration, tenancy policy, incident
response, observability dashboards, or multi-site federation. Those belong to
other repositories in the control plane and are referenced from here only as
stable opaque identifiers. Do not expand scope into adjacent systems or sibling
repositories.

## Quality expectations

Changes should meet the repository's normal code-quality, build, test,
documentation, and cleanup expectations. Run the full build and test suite
before opening a pull request, and keep the working tree clean. The project
builds warning-clean with warnings treated as errors; a change that introduces a
first-party warning is not acceptable.

Invariants are enforced in the library, not only in tests. A change that
weakens a documented invariant, a precondition check or an integrity check
needs to say so explicitly in its description and to update the README.

No test carries a timeout. A hang is a defect to diagnose and fix, never
something to terminate or classify as a pass.

## Attribution

Do not add AI attribution or unintended "Co-authored-by" trailers to commit
messages. The commit author is the authoritative attribution.

## Telemetry

Do not introduce telemetry transmission.
