"""Remember command line. Plain words in, plain words out.

Usage:
  remember save <recording>     transcribe a voice memo and keep it
  remember note "some words"    type a memo instead of speaking it
  remember today                everything you said today, in order
  remember search "doctor"       find memos about something
  remember readback             read today's memos back aloud (printed)
  remember readback 2026-10-08  read back a different day

Your words stay on this machine. Nothing is uploaded anywhere.
"""

import argparse
import sys
from pathlib import Path

from .db import Journal
from .transcribe import MockTranscriber, WhisperTranscriber, default_transcriber

DB_PATH = Path.home() / ".remember" / "remember.db"


def open_journal(db: str | None) -> Journal:
    path = Path(db) if db else DB_PATH
    path.parent.mkdir(parents=True, exist_ok=True)
    return Journal(path)


def cmd_save(args) -> int:
    transcriber = default_transcriber()
    if isinstance(transcriber, MockTranscriber):
        print("note: whisper isn't installed, so I'll store your words "
              "from --as instead of transcribing.")
    journal = open_journal(args.db)
    try:
        if args.as_text:
            text = args.as_text
        else:
            text = transcriber.transcribe(args.recording)
        memo = journal.save(text, audio_path=args.recording or "")
        print(f"kept. ({memo.spoken_time()})")
        print(f'  "{memo.transcript}"')
        return 0
    except (RuntimeError, FileNotFoundError, ValueError) as e:
        print(f"sorry — {e}")
        return 1
    finally:
        journal.close()


def cmd_note(args) -> int:
    journal = open_journal(args.db)
    try:
        memo = journal.save(args.text)
        print(f"kept. ({memo.spoken_time()})")
        return 0
    except ValueError as e:
        print(f"sorry — {e}")
        return 1
    finally:
        journal.close()


def cmd_today(args) -> int:
    journal = open_journal(args.db)
    try:
        memos = journal.today()
        if not memos:
            print("nothing yet today. Say something and I'll keep it.")
            return 0
        print(f"today — {len(memos)} {'memo' if len(memos) == 1 else 'memos'}:\n")
        for m in memos:
            print(f"  {m.local_time.strftime('%-I:%M %p')}  {m.transcript}")
        return 0
    finally:
        journal.close()


def cmd_search(args) -> int:
    journal = open_journal(args.db)
    try:
        memos = journal.search(args.query)
        if not memos:
            print(f'nothing about "{args.query}" yet.')
            return 0
        print(f'about "{args.query}" — newest first:\n')
        for m in memos:
            print(f"  {m.spoken_time()}\n    {m.transcript}\n")
        return 0
    finally:
        journal.close()


def cmd_readback(args) -> int:
    from .db import today_local
    journal = open_journal(args.db)
    try:
        day = args.date or today_local()
        memos = journal.on_date(day)
        if not memos:
            print(f"nothing saved on {day}.")
            return 0
        print(f"here's {day}:\n")
        for m in memos:
            print(f"  {m.local_time.strftime('%-I:%M %p')}. {m.transcript}")
        print(f"\nThat's everything — {len(memos)} "
              f"{'memo' if len(memos) == 1 else 'memos'}.")
        return 0
    finally:
        journal.close()


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(
        prog="remember",
        description="A voice-first memory journal. Your words stay on this machine.")
    parser.add_argument("--db", help="journal database file (default: ~/.remember/remember.db)")
    sub = parser.add_subparsers(dest="cmd", required=True)

    p = sub.add_parser("save", help="transcribe a voice memo and keep it")
    p.add_argument("recording", nargs="?", default="",
                   help="audio file to transcribe")
    p.add_argument("--as", dest="as_text", default="",
                   help="use these words instead of transcribing")
    p.set_defaults(func=cmd_save)

    p = sub.add_parser("note", help="type a memo instead of speaking it")
    p.add_argument("text", help="the words to keep")
    p.set_defaults(func=cmd_note)

    p = sub.add_parser("today", help="everything you said today, in order")
    p.set_defaults(func=cmd_today)

    p = sub.add_parser("search", help="find memos about something")
    p.add_argument("query", help="what to look for")
    p.set_defaults(func=cmd_search)

    p = sub.add_parser("readback", help="read the day's memos back")
    p.add_argument("date", nargs="?", default="",
                   help="which day (YYYY-MM-DD), default today")
    p.set_defaults(func=cmd_readback)

    args = parser.parse_args(argv)
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main())
