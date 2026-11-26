# Third-party components

FileSearcher 0.2.0 uses the components below. FileSearcher's own code is all rights
reserved; third-party rights remain governed by their respective license texts.
This document is an inventory and distribution instruction, not a substitute for
the bundled licenses or a claim of legal certification.

| Component | Pinned version / provenance | License / notices |
| --- | --- | --- |
| Qt Core, GUI, Widgets, Network, Concurrent, SQL and supporting plugins | Qt 6.8.3, qtbase `c07c2d5a527a644d36e7853d55132ae38921682f` | LGPL v3 option; Qt and bundled third-party notices |
| Qt QML / Quick / Quick Controls and associated modules | Qt 6.8.3 SDK revision `2ec235a2235a119887683e55787b93f99dc5f12f` | LGPL v3 option; exact SDK revision differs from the general source-release archive |
| Qt SVG | Qt 6.8.3, `c75099a75e81df35e7347a65e301bbf1011831a5` | LGPL v3 option and bundled notices |
| Qt PDF / PDFium | Qt 6.8.3, `b586c4eb65d8e46ab2c255e1a141676043a650da` | LGPL v3 option for Qt PDF; PDFium BSD and its component licenses, including FreeType, ICU, JPEG, PNG and zlib |
| ONNX Runtime | 1.23.2 official Windows x64 archive, checksum in setup script | MIT and bundled ThirdPartyNotices |
| BGE-small-en-v1.5 | BAAI revision `5c38ec7c405ec4b44b94cc5a9bb96e735b38267a` | MIT; license and model card bundled |
| hnswlib | 0.9.0, vcpkg baseline from vcpkg.json | Apache 2.0 |
| libzip | 1.11.4 | BSD 3-Clause |
| zlib | 1.3.2 | zlib license |
| SQLite and other Qt internals | Exact versions/copyrights in Qt SDK SPDX inventory | SQLite public domain and each component's own terms |
| Microsoft Visual C++ runtime installer | Official VS redistributable supplied with the build toolchain | Microsoft redistributable terms; original signed installer, not a relicensing by FileSearcher |

The package's `licenses` directory contains the vendor notices, SPDX inventories,
Qt GPL v3 and LGPL v3 license texts, and a conservative collection of original
license/copyright/notice/attribution files from the pinned Qt source archives.
This collection includes unused source components and does not assert that all
listed source components are part of the executable. The deployed DLL list and
hashes are in `manifest.json` and build provenance is in `release-metadata.json`.

## Qt library replacement and corresponding source

Qt is dynamically linked. Recipients may replace the Qt DLLs/plugins with modified,
interface-compatible builds. FileSearcher does not enforce DLL signatures or prevent
that replacement. Its verification script will report changed hashes, but it is a
diagnostic tool and is not an application startup requirement. LGPL-required reverse
engineering for debugging modifications to these libraries is permitted, as stated
in LICENSE. FileSearcher's separate proprietary licensing does not remove those rights.

The adjacent `*-dependency-sources` directory/ZIP contains the complete pinned Qt
source archives used for the notice collection, including the Qt PDF/PDFium sources
inside Qt WebEngine's source archive. No Qt WebEngine runtime is shipped. Every
source revision is checked against the installed SDK's SPDX record. Follow Qt's
source build instructions for MSVC x64 and the matching module revisions, then
replace the compatible libraries/plugins in a separate app copy. No Qt source patch
was applied by this project. Both application source and the build instructions
are included in the validation package for inspection, without granting a general
license to FileSearcher's own code.

Before any public binary distribution, make the corresponding source companion
available alongside the binary package, retain these notices and license files,
and complete the independent release checklist. Upstream download links alone are
not treated by this project as a completed source-distribution process.

References: [Qt LGPL terms](https://doc.qt.io/qt-6.8/lgpl.html),
[Qt PDF licensing](https://doc.qt.io/qt-6.8/qtpdf-licensing.html),
[Qt third-party inventory guidance](https://doc.qt.io/qt-6.8/licenses-used-in-qt.html).
