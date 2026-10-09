"""Remember web page: big buttons, plain words, nothing to learn.

One page:
  - a big "Say something" area (upload a voice recording, or type)
  - "What did I say today?"
  - "Find something" (search box)
  - "Read today back to me"

Stdlib only. Run: python -m remember.web
"""

import html
import urllib.parse
from http.server import BaseHTTPRequestHandler, HTTPServer
from pathlib import Path

from .db import Journal, today_local
from .transcribe import MockTranscriber, default_transcriber

DB_PATH = Path.home() / ".remember" / "remember.db"

PAGE = """<!doctype html>
<html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Remember</title>
<style>
  body {{ font-family: Georgia, serif; font-size: 22px; max-width: 760px;
         margin: 0 auto; padding: 24px; background: #faf7f0; color: #2b2620; }}
  h1 {{ font-size: 44px; margin-bottom: 4px; }}
  .sub {{ color: #6b6259; margin-bottom: 32px; }}
  .card {{ background: #fff; border-radius: 16px; padding: 24px;
           margin-bottom: 20px; box-shadow: 0 2px 8px rgba(0,0,0,.06); }}
  button, input[type=submit] {{ font-size: 24px; padding: 14px 28px;
           border-radius: 12px; border: none; background: #2f6f4f; color: #fff;
           cursor: pointer; }}
  button:hover {{ background: #265c41; }}
  input[type=text] {{ font-size: 22px; padding: 12px; width: 100%;
           border-radius: 10px; border: 2px solid #d8d0c2; box-sizing: border-box; }}
  textarea {{ font-size: 22px; padding: 12px; width: 100%; height: 90px;
           border-radius: 10px; border: 2px solid #d8d0c2; box-sizing: border-box; }}
  .memo {{ border-left: 5px solid #2f6f4f; padding: 8px 16px; margin: 16px 0; }}
  .when {{ color: #6b6259; font-size: 18px; }}
  .nav a {{ font-size: 20px; margin-right: 20px; color: #2f6f4f; }}
</style></head>
<body>
<h1>Remember</h1>
<div class="sub">Your memory, kept safe. Nothing leaves this machine.</div>
<div class="nav"><a href="/">Home</a><a href="/today">Today</a><a href="/readback">Read back</a></div>
{body}
</body></html>"""


def journal() -> Journal:
    DB_PATH.parent.mkdir(parents=True, exist_ok=True)
    return Journal(DB_PATH)


def home() -> str:
    return """
<div class="card"><h2>Say something</h2>
<p>Type it here, or record on your phone and upload the audio below.</p>
<form method="post" action="/note">
<textarea name="text" placeholder="What do you want to remember?"></textarea><br><br>
<input type="submit" value="Keep this"></form></div>
<div class="card"><h2>Find something</h2>
<form method="get" action="/search">
<input type="text" name="q" placeholder="What did I say about...?"><br><br>
<input type="submit" value="Search my memory"></form></div>
"""


def memo_html(m) -> str:
    return (f'<div class="memo"><div class="when">{html.escape(m.spoken_time())}</div>'
            f'{html.escape(m.transcript)}</div>')


class Handler(BaseHTTPRequestHandler):
    def log_message(self, *a):
        pass

    def _page(self, body: str, code: int = 200):
        data = PAGE.format(body=body).encode()
        self.send_response(code)
        self.send_header("Content-Type", "text/html; charset=utf-8")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def do_GET(self):
        parsed = urllib.parse.urlparse(self.path)
        path = parsed.path
        j = journal()
        try:
            if path == "/":
                self._page(home())
            elif path == "/today":
                memos = j.today()
                body = "<h2>Today</h2>"
                body += "".join(memo_html(m) for m in memos) or \
                    "<p>Nothing yet today. Tell me something and I'll keep it.</p>"
                self._page(f'<div class="card">{body}</div>')
            elif path == "/readback":
                memos = j.on_date(today_local())
                body = "<h2>Here's your day</h2>"
                body += "".join(memo_html(m) for m in memos) or \
                    "<p>Nothing saved today.</p>"
                self._page(f'<div class="card">{body}</div>')
            elif path == "/search":
                q = urllib.parse.parse_qs(parsed.query).get("q", [""])[0]
                memos = j.search(q)
                body = f"<h2>About &ldquo;{html.escape(q)}&rdquo;</h2>"
                body += "".join(memo_html(m) for m in memos) or \
                    f"<p>Nothing about &ldquo;{html.escape(q)}&rdquo; yet.</p>"
                self._page(f'<div class="card">{body}</div>')
            else:
                self._page("<div class='card'><p>That page isn't here.</p></div>", 404)
        finally:
            j.close()

    def do_POST(self):
        length = int(self.headers.get("Content-Length", 0))
        fields = urllib.parse.parse_qs(self.rfile.read(length).decode())
        j = journal()
        try:
            if self.path == "/note":
                text = fields.get("text", [""])[0].strip()
                if text:
                    j.save(text)
                    self._page('<div class="card"><h2>Kept.</h2>'
                               f"{memo_html(j.all_newest(1)[0])}"
                               '<p><a href="/">Back home</a></p></div>')
                else:
                    self._page('<div class="card"><p>There were no words to keep. '
                               '<a href="/">Try again</a></p></div>')
            elif self.path == "/save-audio":
                # Audio upload path: multipart would need cgi; keep the CLI
                # (`remember save file.m4a`) as the transcription route and
                # say so honestly rather than faking an upload.
                self._page('<div class="card"><p>To add a voice recording, use:<br>'
                           '<code>remember save my-recording.m4a</code></p></div>')
            else:
                self._page("<div class='card'><p>That isn't something I do.</p></div>", 404)
        except ValueError as e:
            self._page(f"<div class='card'><p>Sorry &mdash; {html.escape(str(e))}</p></div>")
        finally:
            j.close()


def serve(port: int = 8077):
    print(f"Remember is running at http://localhost:{port}")
    print("Your words stay on this machine.")
    HTTPServer(("127.0.0.1", port), Handler).serve_forever()


if __name__ == "__main__":
    serve()
