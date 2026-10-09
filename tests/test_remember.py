"""Tests for Remember. Everything here runs without a microphone,
without whisper, and without the network.
"""

import time
from datetime import datetime
from zoneinfo import ZoneInfo

import pytest

from remember.db import Journal, Memo, utc_to_local_date
from remember.transcribe import MockTranscriber, WhisperTranscriber

NY = ZoneInfo("America/New_York")


@pytest.fixture
def journal(tmp_path):
    j = Journal(tmp_path / "test.db")
    yield j
    j.close()


def test_save_and_recall_roundtrip(journal):
    memo = journal.save("the doctor said to take the blue pill after breakfast")
    found = journal.get(memo.id)
    assert found is not None
    assert found.transcript == "the doctor said to take the blue pill after breakfast"
    assert found.audio_path == ""
    assert found.local_date == utc_to_local_date(memo.created_utc)


def test_empty_memo_refused(journal):
    with pytest.raises(ValueError):
        journal.save("   ")


def test_search_finds_right_memo_among_decoys(journal):
    journal.save("the weather is nice today")
    journal.save("doctor appointment on thursday at three")
    journal.save("remember to water the plants")
    journal.save("the DOCTOR called back about test results")
    hits = journal.search("doctor")
    assert len(hits) == 2
    # newest first
    assert hits[0].created_utc >= hits[1].created_utc
    assert all("doctor" in h.transcript.lower() for h in hits)


def test_search_case_insensitive(journal):
    journal.save("Met JEN at the store")
    assert len(journal.search("jen")) == 1
    assert len(journal.search("JEN")) == 1
    assert len(journal.search("Jen")) == 1


def test_search_no_match(journal):
    journal.save("something about gardening")
    assert journal.search("spaceship") == []
    assert journal.search("   ") == []


def test_search_special_chars_dont_break(journal):
    journal.save("100% sure about the 50_50 split")
    assert len(journal.search("100%")) == 1
    assert len(journal.search("50_50")) == 1


def test_local_date_bucketing(journal):
    # 2:30 AM New York time on Oct 9 -> bucket "2026-10-09"
    ts = datetime(2026, 10, 9, 2, 30, tzinfo=NY).timestamp()
    memo = journal.save("late night thought", created_utc=ts)
    assert memo.local_date == "2026-10-09"
    assert utc_to_local_date(ts) == "2026-10-09"


def test_late_night_does_not_leak_to_utc_day(journal):
    # 11:30 PM New York on Oct 9 is Oct 10 in UTC — must still bucket Oct 9
    ts = datetime(2026, 10, 9, 23, 30, tzinfo=NY).timestamp()
    memo = journal.save("almost midnight thought", created_utc=ts)
    assert memo.local_date == "2026-10-09"


def test_on_date_returns_in_order(journal):
    base = datetime(2026, 10, 8, 9, 0, tzinfo=NY).timestamp()
    journal.save("third", created_utc=base + 7200)
    journal.save("first", created_utc=base)
    journal.save("second", created_utc=base + 3600)
    memos = journal.on_date("2026-10-08")
    assert [m.transcript for m in memos] == ["first", "second", "third"]


def test_today_only_returns_today(journal):
    now = time.time()
    journal.save("right now", created_utc=now)
    journal.save("long ago", created_utc=now - 86400 * 30)
    memos = journal.today()
    assert [m.transcript for m in memos] == ["right now"]


def test_spoken_time_is_words(journal):
    ts = datetime(2026, 10, 9, 14, 5, tzinfo=NY).timestamp()
    memo = journal.save("afternoon note", created_utc=ts)
    assert memo.spoken_time() == "October 9, 2026 at 2:05 PM"


def test_mock_transcriber_pipeline(journal):
    t = MockTranscriber({"visit.m4a": "the plumber is coming friday morning"})
    text = t.transcribe("visit.m4a")
    memo = journal.save(text, audio_path="visit.m4a")
    assert memo.transcript == "the plumber is coming friday morning"
    assert memo.audio_path == "visit.m4a"
    assert journal.search("plumber")[0].id == memo.id


def test_mock_transcriber_default(journal):
    t = MockTranscriber()
    assert t.transcribe("anything.wav") == "this is a test memo"


def test_whisper_missing_gives_clear_error(monkeypatch):
    import shutil
    monkeypatch.setattr(shutil, "which", lambda *_: None)
    with pytest.raises(RuntimeError) as e:
        WhisperTranscriber()
    assert "pip install openai-whisper" in str(e.value)


def test_tags_stored(journal):
    memo = journal.save("call the pharmacy", tags="health,errand")
    assert journal.get(memo.id).tags == "health,errand"


def test_persistence_across_reopen(tmp_path):
    p = tmp_path / "reopen.db"
    j1 = Journal(p)
    j1.save("remember the milk")
    j1.close()
    j2 = Journal(p)
    try:
        assert len(j2.search("milk")) == 1
    finally:
        j2.close()
