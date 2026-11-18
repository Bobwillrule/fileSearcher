# Phase 3 — Reliable incremental indexing

Historical phase notes: [Phase 4](PHASE_4_CONTENT_SEARCH.md) now adds local content
extraction and keyword search to this background indexing foundation.

Implemented November 11, 2026. Selected folders now update automatically after
creates, edits, renames, and deletes. All checks use synthetic files in temporary
or ignored build directories.

## Behavior

- Recursive Windows notifications schedule an update for the affected root.
- Events within a 500 ms window are coalesced, with no unbounded postponement
  during a steady stream of changes. Each root has at most one durable work row.
- Startup reconciles every selected root. A five-minute check catches missed
  events and reconnects unavailable or replaced folders.
- Pause cancels the active transaction and saves pending work. Pause persists
  across app restarts. Resume processes saved work; searches remain available.
- Rescan queues all roots immediately, or waits if indexing is paused.
- Removing a root removes its pending work and index membership, leaving files
  intact. Changes under the app's own index folder do not trigger scan loops.
- A root that cannot be fully read retains its previous records and shows its
  error/status. A missing drive does not clear the index.

## Architecture and durability

`DirectoryWatcher` uses one overlapped `ReadDirectoryChangesW` request and one
64 KiB buffer per root, integrated with Qt's event loop through
`QWinEventNotifier`. It does not create a thread per folder. Live watches are
capped at 128; extra roots use periodic checks and the sidebar reports degraded
coverage. Removed/unavailable/replaced watches are closed and recreated as needed.

An empty notification batch, overflow, or failed notification requests a complete
root reconciliation. Microsoft documents that overflow may discard the buffer
and return zero bytes, requiring enumeration to recover. See
[ReadDirectoryChangesW](https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-readdirectorychangesw).
The event handle is reset when rearming, as required for a manual-reset event
used with [QWinEventNotifier](https://doc.qt.io/qt-6/qwineventnotifier.html).

`WorkQueue` stores root ID/path, a monotonically increasing revision, due time,
attempt count, and claimed status in `work.sqlite`. This is separate from the
metadata database so notifications can be committed while the scanner owns a
metadata write transaction. Queue writes use WAL and `synchronous=FULL`. A new
event increments the revision even if the root is already being scanned. A
completed scan removes work only when its revision still matches; later events
always receive another pass.

An interrupted claim becomes pending on restart. Startup also queues every root,
covering changes made while the app was closed and the gap between publishing
metadata and acknowledging work. Reconciliation is idempotent, so either side
of that gap can be replayed. Orphaned queue entries are removed against the
current root list. A per-index process lock prevents competing app writers.

There is one scan worker and one search worker. Failed roots receive at most
three attempts per cycle, with one- then two-second retry delays. Failed work
remains stored after the limit. A new event, manual rescan, startup, or periodic
check begins a new retry cycle. Cancellation does not consume an attempt.

## Reconciliation and identity

The work unit is an affected root, rather than an individual notification path.
This intentionally uses the existing transactional scanner to resolve directory
renames, unpaired rename notifications, hard links, and overflow consistently.
Unchanged metadata rows are not rewritten; membership generations still update.
An affected large root is enumerated again, so this is not an Everything-style
USN/MFT index or an O(1) update for every file event. Finer-grained work and
large-collection performance remain optimization work.

Volume serial and directory file index are checked where available to detect a
root replaced during scanning and to reattach a watcher to the correct directory.
Root accessibility is rechecked before publication. Replacement or disappearance
rolls back the scan; incomplete enumeration never prunes old memberships.
If directory identity is unavailable, accessibility checks and periodic path
reconciliation provide the fallback.

Individual files retain the Phase 2 normalized-path identity. A renamed path is
reconciled as a new path and the old path is removed only after a complete scan;
its database ID need not survive the rename. Hard-link paths remain distinct.
There is no heuristic transfer of an ID between ambiguous hard links. The new
phase has no content embeddings or organization actions that depend on stable
file IDs across renames.

## Cloud files and exclusions

Scanning reads metadata only. Reparse points, system entries, `OFFLINE`,
`RECALL_ON_OPEN`, and `RECALL_ON_DATA_ACCESS` entries are skipped. No file-content
reads or cloud-download requests are made. The app has no hydration opt-in yet.
Tests exercise the offline attribute on a synthetic local file; real cloud
provider accounts and their synchronization behavior have not been validated.

Watcher filtering applies root exclusions to all path components, so events
inside an excluded directory do not cause repeated rescans. The index directory
is protected even when its parent is an indexed root. Periodic checks still
provide recovery when notification coverage is unavailable.

## Validation

Local validation passed all 12 CTest entries in both Debug and Release. After the
final cancellation/shutdown refinements, the three affected checks (metadata,
desktop workflow, and QML startup) passed again in both configurations. The
populated offscreen render exited successfully with no QML diagnostics.

`metadata_indexing` now includes 12 scenarios. The additions verify that replacing
a root during enumeration preserves its published snapshot, and that an offline
file is skipped without reading content and is indexed after the attribute clears.

`desktop_search_workflow` now includes 12 scenarios. Alongside Phase 2 coverage,
the new checks exercise:

- Durable coalescing, interrupted claims, and revision-safe acknowledgement.
- Retry limits, backoff, and cancellation without consuming retries.
- Recursive creates, rapid repeated saves, file/directory renames, and deletes.
- Saved pause state, changes during pause, changes while closed, and startup recovery.
- An unavailable root retaining records and a replacement root recovering automatically.
- Overlapping watched roots and removal of queued work without deleting files.
- A watched parent containing the index without self-triggered scan loops.
- Rejection of a second writer for the same index.
- A real notification-buffer overflow induced by 1,600 long-name fixture files;
  the overflow signal must be observed and reconciliation requested.
- Changes arriving while a scan is running, followed by convergence to the new file set.

The populated interface is rendered from the saved synthetic demo and inspected
for monitoring status, Pause/Resume, Rescan, and unchanged search controls.
The full suite continues to include Phase 1 extraction and model parity checks.

## Remaining scope

Case-sensitive directories, UNC/network shares, independent-machine packaging,
million-file performance, and real cloud-provider behavior remain outside this
phase's validation. Notification reliability is supplemented by reconciliation,
not treated as a complete history of filesystem events. Search may briefly show
the prior snapshot during an update, or retain old records while a root is
unavailable. The app must remain open for live monitoring; restart catches up.

Phase 4 connects text extraction and keyword content search. Semantic search and
file organization remain later phases.
