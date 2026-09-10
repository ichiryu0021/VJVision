"""SQLite backend for dejavu (portable / zero-install fingerprint store).

dejavu ships only MySQL and PostgreSQL handlers.  For the USB-portable
build we cannot depend on a MySQL server, so this module implements the
same interface as :class:`dejavu.database_handler.mysql_database.MySQLDatabase`
on top of a single SQLite file — the entire fingerprint database then
lives next to the executable and can be copied from PC A to PC B as-is.

Hashes are stored as uppercase hex TEXT (dejavu already passes them as
hex strings; MySQL stores UNHEX() bytes, SQLite just keeps the text).
The ``(song_id, offset, hash)`` uniqueness constraint and the ``hash``
index mirror the MySQL schema.

WAL journal mode + a busy timeout make the file safe to use from the
matcher thread and from the per-song indexing worker processes
concurrently.
"""
from __future__ import annotations

import queue
import sqlite3
from typing import Any

from dejavu.base_classes.common_database import CommonDatabase
from dejavu.config.settings import (FIELD_FILE_SHA1, FIELD_FINGERPRINTED,
                                    FIELD_HASH, FIELD_OFFSET, FIELD_SONG_ID,
                                    FIELD_SONGNAME, FIELD_TOTAL_HASHES,
                                    FINGERPRINTS_TABLENAME, SONGS_TABLENAME)


class SQLiteDatabase(CommonDatabase):
    type = "sqlite"

    # -- DDL --------------------------------------------------------------
    CREATE_SONGS_TABLE = f"""
        CREATE TABLE IF NOT EXISTS {SONGS_TABLENAME} (
            {FIELD_SONG_ID}       INTEGER PRIMARY KEY AUTOINCREMENT,
            {FIELD_SONGNAME}      TEXT NOT NULL,
            {FIELD_FINGERPRINTED} INTEGER NOT NULL DEFAULT 0,
            {FIELD_FILE_SHA1}     TEXT NOT NULL,
            {FIELD_TOTAL_HASHES}  INTEGER NOT NULL DEFAULT 0,
            date_created          TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP,
            date_modified         TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP
        );
    """

    CREATE_FINGERPRINTS_TABLE = f"""
        CREATE TABLE IF NOT EXISTS {FINGERPRINTS_TABLENAME} (
            {FIELD_HASH}    TEXT NOT NULL,
            {FIELD_SONG_ID} INTEGER NOT NULL,
            {FIELD_OFFSET}  INTEGER NOT NULL,
            date_created    TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP,
            date_modified   TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP,
            UNIQUE ({FIELD_SONG_ID}, {FIELD_OFFSET}, {FIELD_HASH})
        );
    """
    # The hash index is what makes recognition queries fast. Created
    # separately (IF NOT EXISTS) so an existing db without it upgrades
    # itself on open.
    CREATE_HASH_INDEX = (
        f"CREATE INDEX IF NOT EXISTS ix_{FINGERPRINTS_TABLENAME}_{FIELD_HASH} "
        f"ON {FINGERPRINTS_TABLENAME} ({FIELD_HASH});"
    )

    # -- DML --------------------------------------------------------------
    # INSERT OR IGNORE == MySQL's INSERT IGNORE for the unique (sid,off,hash).
    # dejavu passes hashes as lowercase hexdigest strings, but matching
    # uppercases EVERYTHING (mapper keys are hsh.upper() and MySQL's
    # HEX() returns uppercase).  Normalise to UPPER on write + lookup so
    # the text comparisons match (MySQL compared the UNHEX() bytes and
    # was case-blind; SQLite TEXT comparison is not).
    INSERT_FINGERPRINT = f"""
        INSERT OR IGNORE INTO {FINGERPRINTS_TABLENAME}
            ({FIELD_SONG_ID}, {FIELD_HASH}, {FIELD_OFFSET})
        VALUES (?, UPPER(?), ?);
    """

    INSERT_SONG = f"""
        INSERT INTO {SONGS_TABLENAME}
            ({FIELD_SONGNAME}, {FIELD_FILE_SHA1}, {FIELD_TOTAL_HASHES})
        VALUES (?, UPPER(?), ?);
    """

    SELECT = f"""
        SELECT {FIELD_SONG_ID}, {FIELD_OFFSET}
        FROM {FINGERPRINTS_TABLENAME}
        WHERE {FIELD_HASH} = UPPER(?);
    """

    # Column order matters: CommonDatabase.return_matches iterates
    # ``for hsh, sid, offset in cur``.  The ``%s`` tokens are consumed
    # by CommonDatabase's own ``% ','.join(...)`` formatting (they never
    # reach SQLite) and end up as qmark ``?`` placeholders via IN_MATCH.
    # UPPER() is applied to the PARAMETERS only, so the hash index is
    # still used (no function on the indexed column).
    SELECT_MULTIPLE = f"""
        SELECT {FIELD_HASH}, {FIELD_SONG_ID}, {FIELD_OFFSET}
        FROM {FINGERPRINTS_TABLENAME}
        WHERE {FIELD_HASH} IN (%s);
    """

    SELECT_ALL = (
        f"SELECT {FIELD_SONG_ID}, {FIELD_OFFSET} "
        f"FROM {FINGERPRINTS_TABLENAME};"
    )

    SELECT_SONG = f"""
        SELECT {FIELD_SONGNAME}, {FIELD_FILE_SHA1} AS {FIELD_FILE_SHA1},
               {FIELD_TOTAL_HASHES}
        FROM {SONGS_TABLENAME}
        WHERE {FIELD_SONG_ID} = ?;
    """

    SELECT_NUM_FINGERPRINTS = (
        f"SELECT COUNT(*) AS n FROM {FINGERPRINTS_TABLENAME};"
    )

    SELECT_UNIQUE_SONG_IDS = f"""
        SELECT COUNT({FIELD_SONG_ID}) AS n
        FROM {SONGS_TABLENAME}
        WHERE {FIELD_FINGERPRINTED} = 1;
    """

    SELECT_SONGS = f"""
        SELECT
            {FIELD_SONG_ID},
            {FIELD_SONGNAME},
            {FIELD_FILE_SHA1} AS {FIELD_FILE_SHA1},
            {FIELD_TOTAL_HASHES},
            date_created
        FROM {SONGS_TABLENAME}
        WHERE {FIELD_FINGERPRINTED} = 1;
    """

    DROP_FINGERPRINTS = f"DROP TABLE IF EXISTS {FINGERPRINTS_TABLENAME};"
    DROP_SONGS = f"DROP TABLE IF EXISTS {SONGS_TABLENAME};"

    UPDATE_SONG_FINGERPRINTED = f"""
        UPDATE {SONGS_TABLENAME} SET {FIELD_FINGERPRINTED} = 1
        WHERE {FIELD_SONG_ID} = ?;
    """

    DELETE_UNFINGERPRINTED = (
        f"DELETE FROM {SONGS_TABLENAME} WHERE {FIELD_FINGERPRINTED} = 0;"
    )

    DELETE_SONGS = (
        f"DELETE FROM {SONGS_TABLENAME} WHERE {FIELD_SONG_ID} IN (%s);"
    )

    # Replacement token used by CommonDatabase.return_matches when it
    # builds the ``IN (...)`` list (MySQL's version is UNHEX(%s)).  Each
    # query parameter is uppercased to match the normalised storage.
    IN_MATCH = "UPPER(?)"

    def __init__(self, **options: Any) -> None:
        super().__init__()
        db_path = options.get("path")
        if not db_path:
            raise ValueError("SQLite backend requires database {'path': ...}")
        self._path = str(db_path)
        self._options = options
        # Initialise schema / pragmas once up-front (setup() is called
        # again by Dejavu.__init__ — idempotent with IF NOT EXISTS).
        self.setup()

    # dejavu calls ``with self.cursor() as cur:`` — every context opens a
    # fresh connection.  SQLite connections to a local file are cheap,
    # and this sidesteps cross-thread pooling issues entirely.
    def cursor(self, dictionary: bool = False, **_ignored: Any):
        return _Cursor(self._path, dictionary=dictionary)

    def setup(self) -> None:
        with self.cursor() as cur:
            cur.execute(self.CREATE_SONGS_TABLE)
            cur.execute(self.CREATE_FINGERPRINTS_TABLE)
            cur.execute(self.CREATE_HASH_INDEX)

    def insert_song(self, song_name: str, file_hash: str,
                    total_hashes: int) -> int:
        with self.cursor() as cur:
            cur.execute(self.INSERT_SONG, (song_name, file_hash, total_hashes))
            return cur.lastrowid

    def after_fork(self) -> None:
        # Nothing pooled across processes; each worker makes its own
        # connections.  Required hook for dejavu's pool flow.
        pass

    def __getstate__(self):
        return (self._options,)

    def __setstate__(self, state):
        (options,) = state
        self.__init__(**options)


class _Cursor:
    """Connection-per-context cursor compatible with dejavu's usage.

    ``dictionary=True`` returns rows as plain dicts (mirroring
    mysql-connector's dictionary cursor), which dejavu's
    ``get_songs`` / ``get_song_by_id`` rely on.
    """

    def __init__(self, db_path: str, dictionary: bool = False) -> None:
        self.conn = sqlite3.connect(
            db_path, timeout=30.0, check_same_thread=False,
        )
        # WAL lets the matcher thread read while worker processes write;
        # NORMAL sync is the WAL-recommended durability/perf trade-off.
        self.conn.execute("PRAGMA journal_mode=WAL")
        self.conn.execute("PRAGMA synchronous=NORMAL")
        self.conn.execute("PRAGMA busy_timeout=30000")
        self.conn.execute("PRAGMA foreign_keys=ON")
        if dictionary:
            self.conn.row_factory = self._dict_factory
        self.cur = self.conn.cursor()

    @staticmethod
    def _dict_factory(cursor: sqlite3.Cursor, row: tuple) -> dict:
        return {col[0]: row[idx] for idx, col in enumerate(cursor.description)}

    def __enter__(self) -> sqlite3.Cursor:
        return self.cur

    def __exit__(self, extype, exvalue, traceback) -> None:
        try:
            if extype is None:
                self.conn.commit()
            else:
                self.conn.rollback()
        finally:
            self.cur.close()
            self.conn.close()
