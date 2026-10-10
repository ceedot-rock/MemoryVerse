"""Remember — a voice-first memory journal.

Speak a thought, it's captured with a timestamp. Ask what you said,
it answers. Everything stays on this machine.
"""

from .db import Journal, Memo
from .transcribe import Transcriber, WhisperTranscriber, MockTranscriber

__all__ = ["Journal", "Memo", "Transcriber", "WhisperTranscriber", "MockTranscriber"]
__version__ = "0.1.0"
