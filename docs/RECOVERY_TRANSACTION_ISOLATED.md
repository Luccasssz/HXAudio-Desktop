# Isolated recovery transaction prototype

This work is not a deployable recovery system. The transaction engine has an
explicit isolated-adapter gate and is not connected to MMDevices or uninstall.
The four installation and refresh commands remain disabled. Legacy backups are
not promoted to trusted records and existing cleanup continues to fail closed
when an endpoint backup is present. In particular, the production backup
lifecycle deadlock is not solved by this prototype.

The record contains a schema, transaction ID, endpoint and device instance,
driver INF/package, version and provider, original values, expected installed
values and confirmed installed values. Values encode presence, registry type and
complete bytes. A length-prefixed UTF-16LE representation is SHA-256 hashed via
CNG; the digest detects corruption, not origin. A strict bounded decoder rejects
malformed or truncated records. `TrustedOrigin()` is an adapter contract, not an
implemented production authentication mechanism.

The isolated engine preflights all three-way comparisons before writes, records
intent and progress through a durable adapter, rechecks identity and value around
each write, verifies the final snapshot and archives without deleting the record.
The disposable HKCU adapter writes a single progress value and calls
`RegFlushKey`; child processes are terminated at progress and post-write
boundaries and restarted against the same fixture. The test adapter controls the
fixture's writers; it does not enforce mutual exclusion against an OEM driver.

There is no general registry compare-and-swap. Re-reading before and after a
write cannot prevent an uncooperative writer from changing a value in between.
No live adapter, ACL/authenticity design, genuine endpoint/driver identity
capture, install-phase durable intent, independent archive verification, or
component/SCM transaction is implemented. Production use requires a separately
reviewed synchronization design and real-system validation under explicit
authorization; these tests must not be described as that validation.
