# Restore safety work in progress

This branch is not a deployable uninstall solution. Install and refresh remain
blocked. Existing backups remain untrusted and are never replayed automatically.
Do not execute the installer or uninstaller against a real audio installation.

The offline planner compares presence, registry type and exact bytes, so list
order and embedded terminators matter. It rejects incomplete/mismatched endpoint
and driver identities, duplicate property names and any conflicting property
before producing a plan. It does not read or write the registry. A successful
plan is not evidence that restoration is safe on Windows.

Before enabling restoration, define and implement a versioned, durable journal
with complete original and confirmed installed raw values, identity acquisition,
canonical cryptographic fingerprints and explicit transaction states. Protect its
integrity and access, write and flush intent before mutation, validate completeness
and identity again on restart, and address concurrent OEM writes and the absence
of registry compare-and-swap. Legacy backups cannot acquire trust by conversion.
Also account for orphan backups when endpoints disappear and independently verify
component registrations and service state. Specify an explicit human-reviewed
export/recovery process that never overwrites current data automatically.

`CleanupComplete=1` and an empty DLL path now returns failure because neither is
proof of endpoint or component state. This intentionally prevents some repeat
uninstalls until a read-only verification path is implemented. There is no new
transaction persistence, registry fixture, process-crash recovery, independent
Inno uninstall test, or real Windows audio graph validation on this branch.
