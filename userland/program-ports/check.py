#!/usr/bin/env python3
"""Exercise the real interpreter and installed jq inside Coast.

Creates a uniquely named directory under the supplied writable parent. Does
not require root, external network access, host disks, or optional pip modules.
"""
import asyncio
import concurrent.futures
import dataclasses
import decimal
import gzip
import hashlib
import io
import json
import math
import os
from pathlib import Path
import re
import signal
import sqlite3
import ssl
import subprocess
import sys
import tarfile
import tempfile
import threading
import time
import zipfile

parent = Path(sys.argv[1]) if len(sys.argv) == 2 else Path.home() / 'Documents'
work = Path(tempfile.mkdtemp(prefix='python-port-', dir=parent))
print('PROGRAM_PORTS_START uid=%d directory=%s' % (os.getuid(), work), flush=True)


def passed(name):
    print('PROGRAM_PORTS_PASS ' + name, flush=True)


assert sys.version_info[:3] == (3, 14, 10)
assert decimal.Decimal('0.1') + decimal.Decimal('0.2') == decimal.Decimal('0.3')
assert math.isqrt(10**50) == 10**25
assert re.findall(r'\w+', 'café blue 42') == ['café', 'blue', '42']
@dataclasses.dataclass
class Record:
    name: str
    count: int
assert json.loads(json.dumps(dataclasses.asdict(Record('café', 42)))) == dict(name='café', count=42)
passed('language Unicode JSON regex decimal and dataclasses')

payload = ('Coast café 日本語\n' * 10000).encode()
path = work / 'unicode-文件.txt'
path.write_bytes(payload)
assert path.read_bytes() == payload
with path.open('r+b') as stream:
    stream.seek(70000)
    saved = stream.read(256)
    stream.seek(70000)
    stream.write(b'X' * 256)
    stream.seek(70000)
    assert stream.read(256) == b'X' * 256
    stream.seek(70000)
    stream.write(saved)
replacement = work / 'renamed.txt'
path.rename(replacement)
assert replacement.read_bytes() == payload
link = work / 'linked.txt'
link.symlink_to('renamed.txt')
assert link.resolve() == replacement and link.read_bytes() == payload
passed('real file IO seek Unicode paths rename and symlinks')

assert gzip.decompress(gzip.compress(payload)) == payload
with zipfile.ZipFile(work / 'data.zip', 'w', compression=zipfile.ZIP_DEFLATED) as archive:
    archive.writestr('nested/file.txt', payload)
with zipfile.ZipFile(work / 'data.zip') as archive:
    assert archive.read('nested/file.txt') == payload
with tarfile.open(work / 'data.tar.gz', 'w:gz') as archive:
    archive.add(replacement, arcname='file.txt')
with tarfile.open(work / 'data.tar.gz') as archive:
    assert archive.extractfile('file.txt').read() == payload
passed('gzip ZIP and tar archives with real compression')

with sqlite3.connect(work / 'data.db') as connection:
    connection.execute('create table notes(name text, count integer)')
    connection.execute('insert into notes values(?,?)', ('café', 42))
with sqlite3.connect(work / 'data.db') as connection:
    assert connection.execute('select * from notes').fetchall() == [('café', 42)]
    connection.execute('insert into notes values(?,?)', ('discard', 9))
    connection.rollback()
    assert connection.execute('select count(*) from notes').fetchone()[0] == 1
    assert connection.execute('pragma integrity_check').fetchone()[0] == 'ok'
passed('SQLite commit rollback reopen and database integrity')

ready = threading.Event()
result = []
def worker():
    assert ready.wait(5)
    result.append(hashlib.sha256(payload).hexdigest())
thread = threading.Thread(target=worker)
thread.start()
ready.set()
thread.join(8)
assert not thread.is_alive() and result == [hashlib.sha256(payload).hexdigest()]
with concurrent.futures.ThreadPoolExecutor(max_workers=3) as pool:
    assert list(pool.map(lambda n: hashlib.pbkdf2_hmac('sha256', b'password', b'salt', 1000), range(6))) == [hashlib.pbkdf2_hmac('sha256', b'password', b'salt', 1000)] * 6
passed('threads events futures and thread-safe OpenSSL hashing')

assert ssl.OPENSSL_VERSION.startswith('OpenSSL 3.5.9')
context = ssl.create_default_context()
assert context.verify_mode == ssl.CERT_REQUIRED and context.check_hostname
incoming, outgoing = ssl.MemoryBIO(), ssl.MemoryBIO()
tls = context.wrap_bio(incoming, outgoing, server_hostname='coast.test')
try:
    tls.do_handshake()
except ssl.SSLWantReadError:
    pass
else:
    raise AssertionError('TLS must wait for peer input')
assert len(outgoing.read()) > 100
assert context.cert_store_stats()['x509_ca'] > 0
passed('TLS initialization client handshake and certificate trust store')

child = subprocess.run([sys.executable, '-c', 'import sys;sys.stdout.write(sys.stdin.read().upper());sys.stderr.write("child stderr");sys.exit(7)'], input='café\n', text=True, capture_output=True, timeout=12)
assert child.returncode == 7 and child.stdout == 'CAFÉ\n' and child.stderr == 'child stderr'
script = work / 'script.py'
script.write_text('#!/usr/bin/python3\nprint("PYTHON_SHEBANG_OK")\n')
script.chmod(0o755)
assert subprocess.check_output([str(script)], text=True, timeout=12).strip() == 'PYTHON_SHEBANG_OK'
passed('subprocess pipes Unicode IO exit status and shebang execution')

child = subprocess.Popen([sys.executable, '-c', 'import time;time.sleep(30)'])
child.terminate()
assert child.wait(timeout=8) == -signal.SIGTERM
delivered = []
signal.signal(signal.SIGUSR1, lambda number, frame: delivered.append(number))
os.kill(os.getpid(), signal.SIGUSR1)
time.sleep(.02)
assert delivered == [signal.SIGUSR1]
async def asynchronous():
    await asyncio.sleep(.02)
    return await asyncio.gather(asyncio.to_thread(lambda: 40), asyncio.to_thread(lambda: 2))
assert sum(asyncio.run(asynchronous())) == 42
passed('process termination Python signal handlers and asyncio thread handoff')

query = subprocess.run(['jq', '-ec', '{sum:([.items[].n]|add),names:[.items[].name]}'], input=json.dumps(dict(items=[dict(name='café', n=10), dict(name='blue', n=32)])), capture_output=True, text=True, timeout=12)
assert query.returncode == 0, query.stderr
assert json.loads(query.stdout) == dict(sum=42, names=['café', 'blue'])
query = subprocess.run(['jq', '-en', '"Hello42"|capture("(?<word>[A-Za-z]+)(?<n>[0-9]+)")|.n|tonumber'], capture_output=True, text=True, timeout=12)
assert query.returncode == 0 and query.stdout.strip() == '42', query.stderr
query = subprocess.run(['jq', '-e', '.'], input='{broken', capture_output=True, text=True, timeout=12)
assert query.returncode != 0 and 'parse error' in query.stderr
query = subprocess.run(['jq', '-en', 'false'], capture_output=True, text=True, timeout=12)
assert query.returncode == 1
passed('jq transforms Unicode named regex captures parse errors and exit status')

try:
    Path('/etc/program-port-should-be-denied').write_text('no')
except PermissionError:
    pass
else:
    if os.geteuid():
        raise AssertionError('Unprivileged system write was accepted')
passed('ordinary account filesystem permissions')
print('PROGRAM_PORTS_ALL_PASS', flush=True)
