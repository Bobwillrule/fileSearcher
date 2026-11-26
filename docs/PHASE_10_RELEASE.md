# FileSearcher 0.2.0 release candidate

For a standard Windows install, run `FileSearcher-Setup.exe`, then open **FileSearcher** from Start. Setup installs for the current user under `%LOCALAPPDATA%\Programs\FileSearcher`. If the required Microsoft Visual C++ runtime is missing, Setup requests administrator approval to install the bundled prerequisite. Remove FileSearcher through Windows Installed apps. The local candidate installer is unsigned.

For the portable option, extract `FileSearcher-0.2.0-Portable-x64.zip` into a dedicated folder and run `filesearcher.exe`. Both options contain the local model and document readers. Keep your documents in separate folders. The adjacent dependency-source ZIP is for redistribution and rebuilding Qt. FileSearcher's own code remains all rights reserved. See [package instructions](PACKAGE_VALIDATION.md) and [dependency notices](../THIRD_PARTY_NOTICES.md).

## Upgrade and recovery

Close the old app and launch the new executable from a separate folder. Under the single-instance lock, the first upgrade copies preferences and snapshots the search, organization and work databases into `backups` inside the app data folder shown under **Storage & help**. Allow free space for a second copy of that state. SQLite snapshots include committed records in the write-ahead log. Each backup has hashes and database integrity checks. New installations start without a backup.

If the upgrade stops, restart the same version. A verified pending snapshot lets initialization retry safely. Preserve the backup and current data if verification reports an error. Use the packaged tool to inspect or restore a snapshot into a new folder:

```powershell
.\state_backup.exe --verify 'C:\path\to\backups\snapshot'
.\state_backup.exe --restore 'C:\path\to\backups\snapshot' --destination 'C:\recovery\restored-state'
.\filesearcher.exe --data-dir 'C:\recovery\restored-state'
```

The vector search cache rebuilds from saved vectors. For a downgrade, use the **older app with a separate restored pre-upgrade state directory**. Older executables can predate release markers. Keep both state directories until you verify the results. Backups contain unencrypted indexed text, file paths, and history. Clearing the live index preserves backups. With the app closed you can remove a particular backup from `backups`, or remove the dedicated application folder to uninstall its bundled model and binaries. Personal app state is managed separately.

## Rebuild and verify

Use the [build instructions](../README.md), then run the following from the repository with the pinned dependencies installed. Source preparation downloads from Qt on the development machine. The packager verifies source hashes and includes Qt license notices and corresponding source in separate artifacts.

```powershell
.\.tools\python\Scripts\python.exe tools\prepare_release_sources.py
.\tools\build.ps1 -Preset windows-release -ModelTests
.\tools\package.ps1 -OutputDirectory build\FileSearcher-0.2.0 -Zip
.\tools\verify-package.ps1 -PackageDirectory build\FileSearcher-0.2.0 -ObserveTcp
.\tools\verify-release-lifecycle.ps1 -PackageDirectory build\FileSearcher-0.2.0
```

To produce the installer and portable downloads from that ZIP, use Inno Setup 7.1.1. The output folder must be new. The script checks the package contents against the manifest and writes both artifacts and `SHA256SUMS.txt`.

```powershell
.\tools\build-downloads.ps1 -PortableZip build\FileSearcher-0.2.0.zip -OutputDirectory build\downloads-0.2.0 -Compiler 'C:\path\to\ISCC.exe'
.\tools\verify-setup.ps1 -SetupExe build\downloads-0.2.0\FileSearcher-Setup.exe
```

On the independent clean Windows PC, disconnect networking and run `verify-clean-machine.ps1 -RequireOffline` from the extracted package. Record the machine, runtime prerequisite, fonts, DPI, keyboard navigation and Windows Recycle Bin behavior using the checklist in the package instructions. **This independent test is pending by agreement.**

## Privacy and performance evidence

FileSearcher loads a checksum-pinned BGE model from disk. The inference process disables ONNX Runtime telemetry before creating its inference session. Network libraries can be present as Qt dependencies. The optional TCP observation counts connections during the synthetic package workflow and writes process names and counts to its report. Offline operation on a separate installation is covered by the pending test above.

Supported formats include plain text, source files, PDFs with text, and DOCX body text. OCR is outside the current scope. The Phase 6 synthetic evaluation documents the corpus and ranking method. Phase 7 records memory, cold load and large-library latency, including the hybrid latency target miss. See [retrieval evaluation](PHASE_6_HYBRID_SEARCH.md) and [performance measurements](PHASE_7_USABILITY_PERFORMANCE.md).

## Local results on November 26, 2026

All 23 Release checks passed with model tests enabled. Upgrade recovery passed 99 assertions across six cases, including process termination at the pending-marker and migration-commit boundaries. The portable lifecycle check also upgraded state created by the previous Phase 9 executable, restored the snapshot and verified document and state preservation after uninstall. [Lifecycle results](evaluation/phase10-lifecycle.json)

The packaged workflow exercised document extraction, semantic search, reviewed moves, undo, duplicate recycling and restart. Both diagnostic error streams were empty. Across 74 observations the monitor saw zero IPv4 TCP connections from the three sampled processes. The sampler does not measure IPv6, UDP or connections between samples. [Workflow results](evaluation/phase10-package.json) and [TCP observations](evaluation/phase10-tcp.json)

The application ZIP passed integrity and member hash checks. All four source archives matched their pins and the installed SDK revisions. These validation artifacts contain the local source snapshot taken before the final reports and Git commit. [Archive sizes and SHA-256 hashes](evaluation/phase10-artifacts.json)
