# Remember — the memory companion

## The human story

Robert's memory isn't what it used to be. Thoughts come and go — a doctor's
instruction, a name someone mentioned, something he meant to do Thursday —
and by evening they're gone. His son asked the lab to build him something
simple: a place where a spoken thought is never lost.

Remember is that place. He speaks, it's kept with the date and time. Later
he asks, in his own words, "what did I say about the doctor?" — and it
answers. Every evening it can read the day back to him.

## What it does

- **Keeps what you say.** Speak into a recording (or just type), and it's
  saved with the exact date and time. Big text, plain words, no menus to
  get lost in.
- **Finds what you said.** "What did I say about the plumber?" searches
  everything you've ever told it and shows the newest first.
- **Reads the day back.** One button: "here's your day" — every memo in
  order, morning to night.
- **Keeps it private.** Everything lives on his own machine. Nothing is
  uploaded, nothing is sent anywhere, no account needed. His words are
  his.

## How dad uses it

1. He has a thought — he records it on his phone, or types it on the page.
2. It's kept, stamped with the time.
3. When he wonders — "what did the doctor say?" — he asks, and it answers.
4. At night: "read me back today."

That's the whole thing. If it needs explaining beyond that, it's too
complicated.

## What's real vs what's the vision (honest)

**Real today:** the working app — capture (audio file or typed), local
storage, plain-words search, daily readback, the big-button web page.
Tested: 16 automated checks, all passing.

**The vision (concept, not built):** Amazon's Bee is a wearable AI
companion that hears your day. Remember is built for that world — the day
Bee hardware is on his shirt, this is the memory it should keep: local,
private, his. We don't have Bee hardware or a Bee SDK; nothing here
pretends otherwise.

**One real step short:** voice-to-text uses the Whisper program, which
isn't bundled — one install command (`pip install openai-whisper`) and
it runs fully offline after that. Until then, typed notes and the
`--as` flag work fine, and the transcription path is tested with a
stand-in.

## Running it

```bash
cd ~/workspace/remember
python -m remember.cli note "doctor appointment thursday at three"
python -m remember.cli save my-recording.m4a     # needs whisper installed
python -m remember.cli today
python -m remember.cli search "doctor"
python -m remember.cli readback
python -m remember.web      # big-button page at http://localhost:8077
python -m pytest tests/     # the 16 checks
```

## Privacy

Local-first by design: SQLite on his machine, transcription on his
machine, web page served only to his machine (127.0.0.1). There is no
network code in this app — no API keys, no accounts, no analytics.
