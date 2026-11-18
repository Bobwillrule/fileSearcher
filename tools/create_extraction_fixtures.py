"""Generate synthetic parser fixtures under build/fixtures; no external dependencies."""
from pathlib import Path
from zipfile import ZipFile
from pypdf import PdfReader, PdfWriter
from PIL import Image, ImageDraw

DESTINATION = Path(__file__).resolve().parents[1] / "build" / "fixtures"


def pdf(path: Path, text: str | None) -> None:
    stream = (f"BT /F1 12 Tf 40 100 Td ({text}) Tj ET" if text else "").encode("ascii")
    objects = [
        b"<< /Type /Catalog /Pages 2 0 R >>",
        b"<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
        b"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 300 150] /Resources << /Font << /F1 4 0 R >> >> /Contents 5 0 R >>",
        b"<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>",
        b"<< /Length " + str(len(stream)).encode() + b" >>\nstream\n" + stream + b"\nendstream",
    ]
    data = bytearray(b"%PDF-1.4\n")
    offsets = [0]
    for number, body in enumerate(objects, 1):
        offsets.append(len(data))
        data.extend(f"{number} 0 obj\n".encode() + body + b"\nendobj\n")
    xref = len(data)
    data.extend(f"xref\n0 {len(objects) + 1}\n0000000000 65535 f \n".encode())
    for offset in offsets[1:]:
        data.extend(f"{offset:010d} 00000 n \n".encode())
    data.extend(f"trailer\n<< /Size {len(objects) + 1} /Root 1 0 R >>\nstartxref\n{xref}\n%%EOF\n".encode())
    path.write_bytes(data)


def main() -> None:
    DESTINATION.mkdir(parents=True, exist_ok=True)
    (DESTINATION / "notes.txt").write_text("The Master Theorem solves divide-and-conquer recurrences.", encoding="utf-8")
    (DESTINATION / "unknown.filesearcherfixture").write_bytes(b"Synthetic unsupported file")
    (DESTINATION / "malformed.pdf").write_bytes(b"%PDF-1.4\nDeliberately incomplete")
    pdf(DESTINATION / "text.pdf", "The Master Theorem solves recurrences.")
    pdf(DESTINATION / "no_text.pdf", None)  # Blank, not a substitute for a scanned-PDF fixture.
    image = Image.new("RGB", (800, 200), "white")
    ImageDraw.Draw(image).text((30, 60), "Synthetic scanned lecture: Master Theorem", fill="black")
    image.save(DESTINATION / "scanned.pdf", "PDF")
    writer = PdfWriter()
    writer.append(PdfReader(DESTINATION / "text.pdf"))
    writer.encrypt("fixture-password")
    writer.write(DESTINATION / "encrypted.pdf")
    (DESTINATION / "malformed.docx").write_bytes(b"not a zip file")
    with ZipFile(DESTINATION / "oversized.docx", "w", compression=8) as archive:
        archive.writestr("word/document.xml", "x" * (9 * 1024 * 1024))
    with ZipFile(DESTINATION / "notes.docx", "w") as archive:
        archive.writestr("[Content_Types].xml", '<?xml version="1.0"?><Types xmlns="http://schemas.openxmlformats.org/package/2006/content-types"><Default Extension="xml" ContentType="application/xml"/><Default Extension="rels" ContentType="application/vnd.openxmlformats-package.relationships+xml"/><Override PartName="/word/document.xml" ContentType="application/vnd.openxmlformats-officedocument.wordprocessingml.document.main+xml"/></Types>')
        archive.writestr("_rels/.rels", '<?xml version="1.0"?><Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships"><Relationship Id="rId1" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument" Target="word/document.xml"/></Relationships>')
        archive.writestr("word/document.xml", '<?xml version="1.0"?><w:document xmlns:w="http://schemas.openxmlformats.org/wordprocessingml/2006/main"><w:body><w:p><w:r><w:t>Dynamic programming reuses overlapping subproblems.</w:t></w:r></w:p></w:body></w:document>')
    print(DESTINATION)


if __name__ == "__main__":
    main()
