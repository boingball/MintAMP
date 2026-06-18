#!/usr/bin/env python3
from pathlib import Path

path = Path("amiga_mp3dec.c")
text = path.read_text(encoding="utf-8")
old = """\t/* First request cancellation for the entire ring, then reap it in a second
\t * pass.  Waiting one slot at a time lets audio.device advance the next queued
\t * write between AbortIO calls, which is exactly the Stop hang seen with the
\t * old three-request mono ring on real hardware. */
"""
new = """\t/* First request cancellation for the entire ring, then reap it in a second
\t * pass.  Waiting one slot at a time lets audio.device advance the next queued
\t * write between AbortIO calls and can prolong or stall Stop while the queue is
\t * being unwound. */
"""
if text.count(old) != 1:
    raise SystemExit("expected cleanup comment not found exactly once")
path.write_text(text.replace(old, new, 1), encoding="utf-8")
