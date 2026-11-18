# Phase 2 — Metadata index and ordinary search

Historical phase notes: [Phase 3](PHASE_3_INCREMENTAL.md) now adds automatic updates,
startup reconciliation, and persistent pause/resume to this foundation.

Implemented November 11, 2026. The desktop application now indexes explicitly
selected folders and searches saved file metadata. All validation uses synthetic
fixtures in temporary or ignored build directories.

## User-visible behavior

- Add local folders, persist root settings, edit per-root exclusions, and remove
  roots without modifying original files.
- Scan metadata recursively in a background worker, display progress, cancel,
  and rescan. Restart loads the saved index immediately.
- Search literal filename/path fragments, including punctuation and Unicode.
  Exact filenames rank before prefixes and substring matches. Both Windows and
  forward-slash path separators work.
- Combine extension, indexed-root, byte-size, and recent-modification filters.
- Display name, full path, extension, bytes, modified time, and metadata status.
  Open through the default Windows application or reveal in Explorer; unavailable
  files show a rescan message. These actions require an explicit user gesture.
- Keep search running against the published index while a scan is in progress.
  Show a total count and at most 500 results, with a refine-search hint.

## Implementation decisions

`metadata_core` owns the Qt SQL / SQLite store and Windows scanner. The controller
owns separate single-worker pools for scans and searches; workers create their
own database connections. QML sends inputs and renders result records. Queries
are debounced for 180 ms; generation checks prevent an earlier search from
replacing a newer one. Invalid size ranges keep results empty until corrected.

SQLite schema version 1 has `roots`, `files`, and `root_files` membership tables.
Unique normalized path keys prevent duplicate results across overlapping roots.
Hard links remain distinct paths. File IDs are never recycled after deletion,
so a stale result cannot open an unrelated file through a reused database ID.
Path comparison assumes ordinary case-insensitive Windows directories.

Each root is scanned in one transaction using a fresh generation token. A
successful complete scan removes stale memberships and orphaned records. An
incomplete scan preserves previously indexed records, publishes readable entries,
and marks the root partial. Cancellation rolls back the current root. Earlier
completed roots in a multi-root batch remain committed. WAL allows searches to
read the prior published state during a scan. Startup changes an interrupted
`Scanning` status to a rescan notice; it does not automatically scan.

The scanner uses wide Windows enumeration/attribute APIs and extended-length
paths. It never opens file contents, so an exclusive content lock does not prevent
metadata indexing. Reparse points, junctions, symlinks, system-marked entries,
and the application's own index directory are skipped. Exclusions match a
basename or root-relative path and use Qt wildcard semantics; `/` separates
relative path components. No archive or document parsers run during this phase.

Search uses bound SQL parameters and literal LIKE escaping, rather than FTS word
matching. This preserves short filename fragments and punctuation. Name/type
indexes support storage/query evolution; the substring candidate query currently
scans metadata rows. Larger-scale query optimization belongs to Phase 7.

No new external AI dependencies or paid services were added. Qt SQL's bundled
SQLite driver and Qt Concurrent come from the existing Qt installation.

## Acceptance coverage

Final local verification: **12/12 CTest entries passed in both Debug and Release**
with `-ModelTests`. The Release interface rendered successfully with the saved
synthetic index and no QML diagnostics. Placeholder/indicator contrast was
corrected after inspecting that render.

`metadata_indexing` covers ten integration cases:

- Repeated scans, overlapping roots, persistent settings/results, and safe root removal.
- Exact/prefix ranking, literal wildcard punctuation, Unicode, path separators,
  parameterized filters, and injection-shaped text treated as ordinary text.
- Cancellation rollback and concurrent readers seeing the prior snapshot.
- Missing roots, recovery, and stale-file reconciliation.
- Exclusively locked files and two hard-link paths.
- Junction rejection and skipping without traversing the target.
- Paths over 260 characters, exclusions, and index-directory protection.
- An access-denied subdirectory with other entries still searchable.
- A file disappearing during enumeration and a subsequent successful reconciliation.
- Changed sizes, edited exclusions, and non-reuse of removed record IDs.

`desktop_search_workflow` covers two controller-level scenarios: asynchronous
scan/search, latest-query delivery, size validation across rescans, exclusion
edits, restart, missing-file open/reveal errors, safe root removal, protection of
the index directory, and shutdown while work is queued. The QML startup test
loads an isolated temporary index. A populated offscreen screenshot is checked
using six real synthetic files, not fabricated UI result rows.

The full suite also retains Phase 1's vector, extraction, and embedding checks.
Release verification exposed intermittent empty responses during PDF-worker
shutdown. Worker replies are now flushed before teardown; document coverage
includes 30 repeated malformed-PDF responses to guard this behavior.

Windows shell integration is compiled against `ShellExecuteExW` and
`SHOpenFolderAndSelectItems`. Tests verify the unavailable-file path without
launching external applications. Default-app associations and Explorer behavior
on an independent Windows installation remain manual release checks.

## Performance sample

On this development machine, a Release run inserted 50,000 synthetic metadata
records in 5.0 seconds. This measures database insertion, not filesystem scan
speed. Each query was measured 25 times, including count, ranking, and conversion
of up to 500 result rows. Times are local measurements, not release guarantees.

| Query | Matches | Median | 95th percentile |
| --- | ---: | ---: | ---: |
| `report-00001.cpp` | 1 | 83.6 ms | 88.9 ms |
| `report-00` | 1,000 | 76.1 ms | 90.7 ms |
| `001` | 200 | 82.8 ms | 100.9 ms |
| `synthetic/report` | 50,000 | 83.4 ms | 123.0 ms |
| `absent` | 0 | 80.7 ms | 104.8 ms |
| Empty query | 50,000 | 37.3 ms | 59.0 ms |

Reproduce after building with the Qt runtime on PATH:

```powershell
.\build\windows-release\metadata_probe.exe --benchmark
```

The probe creates and removes its own temporary database. It does not crawl
personal folders or create 50,000 physical files.

## Remaining scope

Phase 3 adds directory watchers, durable incremental work, retry/reconciliation,
and pause/resume. Until then, file changes require a manual rescan. Results may
be stale if a root is unavailable or a scan was partial; this is shown at the
root level. No text extraction, embeddings, content search, organization, or
duplicate detection is connected to the desktop UI yet.

Case-sensitive Windows directories, network-share indexing, cloud hydration,
removable-drive policies, million-file performance, and standalone packaging
are not validated for this phase. UNC roots are rejected; reparse-point cloud
placeholders are skipped. Existing metadata-only handling never requests file
content downloads. The current validation is on this Windows machine, not a
clean independent installation.
