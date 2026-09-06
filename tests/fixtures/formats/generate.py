from __future__ import annotations

import gzip
import io
import sqlite3
import subprocess
import sys
import tarfile
import zipfile
from pathlib import Path


ROOT = Path(__file__).resolve().parent
TIMESTAMP = (2024, 1, 2, 3, 4, 6)


class NonSeekableBytesIO(io.BytesIO):
    def seekable(self) -> bool:
        return False

    def seek(self, *args: object, **kwargs: object) -> int:
        raise io.UnsupportedOperation("seek")


def zip_info(
    name: str,
    compression: int,
    timestamp: tuple[int, int, int, int, int, int] = TIMESTAMP,
) -> zipfile.ZipInfo:
    info = zipfile.ZipInfo(name, timestamp)
    info.compress_type = compression
    info.external_attr = 0o100644 << 16
    return info


def write_zip(path: Path, changed: bool) -> None:
    with zipfile.ZipFile(path, "w", allowZip64=True) as archive:
        archive.writestr(zip_info("stored.txt", zipfile.ZIP_STORED), "shared vendored payload\n")
        archive.writestr(
            zip_info("deflated.txt", zipfile.ZIP_DEFLATED),
            "changed archive value\n" if changed else "original archive value\n",
            compresslevel=6,
        )
        with archive.open(zip_info("zip64.txt", zipfile.ZIP_STORED), "w", force_zip64=True) as member:
            member.write(b"small entry with a ZIP64 local header\n")


def write_stored_zip(path: Path) -> None:
    with zipfile.ZipFile(path, "w", compression=zipfile.ZIP_STORED) as archive:
        archive.writestr(zip_info("one.txt", zipfile.ZIP_STORED), "one\n")
        archive.writestr(zip_info("two.txt", zipfile.ZIP_STORED), "two\n")


def write_descriptor_zip(path: Path) -> None:
    output = NonSeekableBytesIO()
    with zipfile.ZipFile(output, "w", compression=zipfile.ZIP_DEFLATED) as archive:
        archive.writestr(zip_info("descriptor.txt", zipfile.ZIP_DEFLATED), "descriptor payload\n")
    path.write_bytes(output.getvalue())


def write_volatile_zip(path: Path, timestamp: tuple[int, int, int, int, int, int]) -> None:
    with zipfile.ZipFile(path, "w", compression=zipfile.ZIP_STORED) as archive:
        archive.writestr(zip_info("same.txt", zipfile.ZIP_STORED, timestamp), "same contents\n")


def tar_member(name: str, contents: bytes) -> tarfile.TarInfo:
    info = tarfile.TarInfo(name)
    info.size = len(contents)
    info.mode = 0o644
    info.uid = 0
    info.gid = 0
    info.mtime = 1_700_000_000
    return info


def write_tar(path: Path) -> None:
    long_name = "nested/" + "long-directory-name/" * 7 + "payload.txt"
    contents = b"pax archive payload\n"
    with tarfile.open(path, "w", format=tarfile.PAX_FORMAT) as archive:
        archive.addfile(tar_member(long_name, contents), io.BytesIO(contents))


def write_tar_wrappers(path: Path) -> None:
    contents = path.read_bytes()
    (ROOT / "pax.tar.gz").write_bytes(gzip.compress(contents, compresslevel=9, mtime=0))
    subprocess.run(
        [
            "zstd",
            "--quiet",
            "--force",
            "-19",
            str(path),
            "-o",
            str(ROOT / "pax.tar.zst"),
        ],
        check=True,
    )


def write_sqlite(path: Path) -> None:
    path.unlink(missing_ok=True)
    connection = sqlite3.connect(path)
    connection.executescript(
        """
        PRAGMA page_size = 512;
        VACUUM;
        CREATE TABLE packages(id INTEGER PRIMARY KEY, name TEXT NOT NULL, version TEXT NOT NULL);
        CREATE TABLE files(id INTEGER PRIMARY KEY, package_id INTEGER NOT NULL, path TEXT NOT NULL);
        INSERT INTO packages VALUES(1, 'zlib', '1.3.1');
        INSERT INTO packages VALUES(2, 'sqlite', '3.46');
        INSERT INTO files VALUES(1, 1, 'lib/libz.a');
        INSERT INTO files VALUES(2, 2, 'lib/libsqlite3.a');
        """
    )
    connection.commit()
    connection.close()


def write_parquet(path: Path) -> None:
    import pyarrow as pa
    import pyarrow.parquet as pq

    schema = pa.schema(
        [
            pa.field("id", pa.int32(), nullable=False),
            pa.field("category", pa.string(), nullable=False),
            pa.field("value", pa.float64(), nullable=False),
        ]
    )
    writer = pq.ParquetWriter(
        path,
        schema,
        compression={"id": "NONE", "category": "gzip", "value": "zstd"},
        use_dictionary=["category"],
        data_page_version="1.0",
        write_statistics=True,
    )
    writer.write_table(
        pa.table({"id": [1, 2, 3], "category": ["alpha", "beta", "alpha"], "value": [1.0, 2.0, 3.0]}, schema=schema)
    )
    writer.write_table(
        pa.table({"id": [4, 5, 6], "category": ["beta", "gamma", "beta"], "value": [4.0, 5.0, 6.0]}, schema=schema)
    )
    writer.close()


def main() -> None:
    write_zip(ROOT / "archive-a.zip", False)
    write_zip(ROOT / "archive-b.zip", True)
    write_stored_zip(ROOT / "stored.zip")
    write_descriptor_zip(ROOT / "descriptor.zip")
    write_volatile_zip(ROOT / "volatile-a.zip", (2024, 1, 2, 3, 4, 6))
    write_volatile_zip(ROOT / "volatile-b.zip", (2025, 2, 4, 5, 6, 8))
    write_tar(ROOT / "pax.tar")
    if "--wrappers" in sys.argv:
        write_tar_wrappers(ROOT / "pax.tar")
    write_sqlite(ROOT / "sample.sqlite")
    if "--parquet" in sys.argv:
        write_parquet(ROOT / "sample.parquet")


if __name__ == "__main__":
    main()
