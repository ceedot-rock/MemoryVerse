"""Storage for Remember: one sqlite database, one table, no surprises.

Each memo stores:
  - id: auto-increment
  - created_utc: unix epoch seconds (UTC) — the single source of truth
  - local_date: YYYY-MM-DD in America/New_York — the day bucket dad sees
  - transcript: the words
  - audio_path: where the original recording lives (may be empty for typed notes)
  - tags: optional comma-separated labels

"Today" and date buckets always use America/New_York, because that's
where dad lives. UTC is stored so the record is unambiguous.
"""

import sqlite3
import time
from dataclasses import dataclass
from datetime import datetime, timezone
from pathlib import Path
from zoneinfo import ZoneInfo

LOCAL_TZ = ZoneInfo("America/New_York")

SCHEMA = """
CREATE TABLE IF NOT EXISTS memos (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    created_utc REAL NOT NULL,
    local_date TEXT NOT NULL,
    transcript TEXT NOT NULL,
    audio_path TEXT NOT NULL DEFAULT '',
    tags TEXT NOT NULL DEFAULT ''
);
CREATE INDEX IF NOT EXISTS idx_memos_date ON memos(local_date);
CREATE INDEX IF NOT EXISTS idx_memos_created ON memos(created_utc);
"""


@dataclass
class Memo:
    id: int
    created_utc: float
    local_date: str
    transcript: str
    audio_path: str = ""
    tags: str = ""

    @property
    def local_time(self) -> datetime:
        return datetime.fromtimestamp(self.created_utc, tz=LOCAL_TZ)

    def spoken_time(self) -> str:
        """'October 9, 2026 at 2:14 AM' — words, not numbers."""
        return self.local_time.strftime("%B %-d, %Y at %-I:%M %p")


def utc_to_local_date(ts: float) -> str:
    return datetime.fromtimestamp(ts, tz=LOCAL_TZ).strftime("%Y-%m-%d")


def today_local() -> str:
    return datetime.now(tz=LOCAL_TZ).strftime("%Y-%m-%d")


class Journal:
    def __init__(self, path: str | Path = "remember.db"):
        self.path = Path(path)
        self.conn = sqlite3.connect(self.path)
        self.conn.row_factory = sqlite3.Row
        self.conn.executescript(SCHEMA)

    def save(self, transcript: str, audio_path: str = "", tags: str = "",
             created_utc: float | None = None) -> Memo:
        text = transcript.strip()
        if not text:
            raise ValueError("nothing to save — the memo was empty")
        ts = created_utc if created_utc is not None else time.time()
        cur = self.conn.execute(
            "INSERT INTO memos (created_utc, local_date, transcript, audio_path, tags)"
            " VALUES (?, ?, ?, ?, ?)",
            (ts, utc_to_local_date(ts), text, audio_path, tags),
        )
        self.conn.commit()
        return self.get(cur.lastrowid)

    def get(self, memo_id: int) -> Memo | None:
        row = self.conn.execute(
            "SELECT * FROM memos WHERE id = ?", (memo_id,)).fetchone()
        return Memo(**dict(row)) if row else None

    def today(self) -> list[Memo]:
        rows = self.conn.execute(
            "SELECT * FROM memos WHERE local_date = ? ORDER BY created_utc",
            (today_local(),)).fetchall()
        return [Memo(**dict(r)) for r in rows]

    def on_date(self, local_date: str) -> list[Memo]:
        rows = self.conn.execute(
            "SELECT * FROM memos WHERE local_date = ? ORDER BY created_utc",
            (local_date,)).fetchall()
        return [Memo(**dict(r)) for r in rows]

    def search(self, query: str, limit: int = 20) -> list[Memo]:
        """Plain-words search, case-insensitive, newest first."""
        q = query.strip()
        if not q:
            return []
        rows = self.conn.execute(
            "SELECT * FROM memos WHERE transcript LIKE ? ESCAPE '\\'"
            " ORDER BY created_utc DESC LIMIT ?",
            ("%" + q.replace("\\", "\\\\").replace("%", "\\%").replace("_", "\\_") + "%",
             limit)).fetchall()
        return [Memo(**dict(r)) for r in rows]

    def all_newest(self, limit: int = 50) -> list[Memo]:
        rows = self.conn.execute(
            "SELECT * FROM memos ORDER BY created_utc DESC LIMIT ?", (limit,)).fetchall()
        return [Memo(**dict(r)) for r in rows]

    def close(self):
        self.conn.close()
