# Cleanup retry and remaining transaction work

The Inno uninstall callback calls `purge` once. `purge` owns APO cleanup,
virtual backend cleanup (including engine bypass cleanup), and finalization in that order.
It stops on the first error. Inno deletes files only after the callback returns.
Recovery settings and backups are retained. A later retry must revalidate APO
cleanup; `CleanupComplete` and an absent DLL path are not evidence by themselves.

Read-only revalidation enumerates current render endpoint FxProperties, checks
for the HXAudio CLSID, fails on unreadable/malformed values or any endpoint
backup, validates every retained component backup against read-only device-node
status, checks COM registrations for the machine and current user in both views,
and requires the HXAudio guard service to be absent. It cannot prove OEM effects
not recorded by a legacy backup were restored, discover another Windows user's
per-user COM registration, or prevent concurrent endpoint/driver changes during
or after inspection. This is not a proof of real audio graph correctness.

The isolated Inno harness compiles this production script with a separate app ID,
fake executables, a temporary install directory, and setup/registry/certificate
side effects disabled. The production uninstall callback remains the same.
It exercises failure and retry, not production Windows audio APIs. The registry
verifier test uses a volatile HKCU subtree, with no system endpoint or SCM access.

Future trusted format (not yet implemented): schema and transaction versions;
installation nonce; endpoint ID and device-instance ID; driver provider, INF,
version, date and package identity; all touched value names, presence, type,
length and exact original and expected installed bytes; canonical SHA-256 digest
over length-prefixed fields; state and per-property progress. A digest detects
corruption but is not authentication: restrict write access and define a trust
model rather than treating a hash as proof of provenance.

Persist and flush prepared intent before any mutation. After installation, read
back every expected value and identity and persist a verified state. Recovery
must check identity and every property, durably journal each intended transition,
re-read immediately before and after writing, and retain records on any conflict.
Windows registry has no generic compare-and-swap, so concurrent OEM changes and
crashes between checks and writes require an explicit coordination design and
fault injection. Legacy records cannot become trusted through conversion alone.
No such journal, identity acquisition, transactional restoration, or automatic
legacy migration is enabled by this branch.
