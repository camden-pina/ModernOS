#!/usr/bin/python
"""Deliver development notifications only to the private persistent outbox."""
import os
from pathlib import Path
import sys
import time
import uuid

os.umask(0o077)
data = sys.stdin.buffer.read(2 * 1024 * 1024 + 1)
if len(data) > 2 * 1024 * 1024:
    raise SystemExit('Local mail exceeds the size limit')
path = Path('/data/outbox')/(str(time.time_ns())+'-'+uuid.uuid4().hex+'.eml')
with path.open('xb') as stream:
    stream.write(data)
