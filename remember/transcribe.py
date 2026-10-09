"""Transcription for Remember: speech to text, on this machine only.

Two implementations behind one interface:

- WhisperTranscriber: shells out to the local `whisper` CLI
  (openai-whisper, no API key, no network). Words never leave the machine.
  Used when the CLI is installed.
- MockTranscriber: returns a canned transcript. Used for tests and for
  machines where whisper isn't installed yet.

Install whisper for real use:
    pip install openai-whisper
(the first run downloads a model into ~/.cache/whisper; after that,
transcription is fully offline)
"""

import shutil
import subprocess
from pathlib import Path


class Transcriber:
    def transcribe(self, audio_path: str | Path) -> str:
        raise NotImplementedError

    @property
    def name(self) -> str:
        return type(self).__name__


class WhisperTranscriber(Transcriber):
    def __init__(self, model: str = "turbo"):
        if shutil.which("whisper") is None:
            raise RuntimeError(
                "the 'whisper' program isn't installed. "
                "Install it with: pip install openai-whisper"
            )
        self.model = model

    def transcribe(self, audio_path: str | Path) -> str:
        audio_path = Path(audio_path)
        if not audio_path.exists():
            raise FileNotFoundError(f"no recording found at {audio_path}")
        out_dir = audio_path.parent
        proc = subprocess.run(
            ["whisper", str(audio_path), "--model", self.model,
             "--output_format", "txt", "--output_dir", str(out_dir),
             "--fp16", "False"],
            capture_output=True, text=True, timeout=600,
        )
        if proc.returncode != 0:
            raise RuntimeError(
                "couldn't transcribe that recording: "
                + (proc.stderr.strip() or "unknown error"))
        txt_file = out_dir / (audio_path.stem + ".txt")
        text = txt_file.read_text(encoding="utf-8").strip()
        txt_file.unlink(missing_ok=True)
        if not text:
            raise RuntimeError("the recording came back with no words in it")
        return text


class MockTranscriber(Transcriber):
    """Pretend transcriber for tests: maps filenames to canned words."""

    def __init__(self, mapping: dict[str, str] | None = None,
                 default: str = "this is a test memo"):
        self.mapping = mapping or {}
        self.default = default

    def transcribe(self, audio_path: str | Path) -> str:
        name = Path(audio_path).name
        return self.mapping.get(name, self.default)


def default_transcriber() -> Transcriber:
    """Whisper when it's here, otherwise the mock (says so honestly)."""
    if shutil.which("whisper"):
        return WhisperTranscriber()
    return MockTranscriber(default="[no recording transcribed — whisper not installed]")
