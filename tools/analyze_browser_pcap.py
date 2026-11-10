#!/usr/bin/env python3
"""Summarize the owned QEMU Ethernet capture, without external packages."""
import collections
import json
from pathlib import Path
import socket
import struct
import sys

data = Path(sys.argv[1]).read_bytes()
endian = '<' if data[:4] == b'\xd4\xc3\xb2\xa1' else '>'
at = 24; flows = {}; dns = []
while at + 16 <= len(data):
    sec, usec, size, _ = struct.unpack_from(endian + 'IIII', data, at); at += 16
    if at + size > len(data): break
    packet = data[at:at + size]; at += size
    if len(packet) < 34 or packet[12:14] != b'\x08\x00': continue
    ip = packet[14:]; header = (ip[0] & 15) * 4
    src, dst = socket.inet_ntoa(ip[12:16]), socket.inet_ntoa(ip[16:20])
    payload = ip[header:int.from_bytes(ip[2:4], 'big')]; now = sec + usec / 1000000
    if ip[9] == 17 and len(payload) >= 20 and 53 in struct.unpack_from('!HH', payload):
        body = payload[8:]; pos = 12; labels = []
        while pos < len(body) and body[pos] and body[pos] < 64:
            n = body[pos]; labels.append(body[pos+1:pos+1+n].decode(errors='replace')); pos += n + 1
        dns.append(dict(at=now, src=src, dst=dst, id=int.from_bytes(body[:2], 'big'), response=bool(body[2]&128), name='.'.join(labels), qtype=int.from_bytes(body[pos+1:pos+3], 'big')))
    if ip[9] != 6 or len(payload) < 20: continue
    sport, dport, seq, ack = struct.unpack_from('!HHII', payload)
    tcpheader = (payload[12] >> 4) * 4
    size = len(payload) - tcpheader
    if len(sys.argv) > 2 and sys.argv[2] in [str(sport), str(dport)]:
        print(now, src, sport, '->', dst, dport, 'seq', seq, 'ack', ack, 'flags', hex(payload[13]), 'window', int.from_bytes(payload[14:16], 'big'), 'bytes', size)
    endpoints = sorted([(src, sport), (dst, dport)])
    key = str(endpoints)
    flow = flows.setdefault(key, dict(first=now, last=now, packets=0, bytes=collections.Counter(), zero_windows=0, retransmit_bytes=0, max_data_gap=0, last_data={}, seen=set(), window_scale={}))
    flow['last'] = now; flow['packets'] += 1
    direction = src + ':' + str(sport)
    if payload[13] & 2:
        opt = 20
        while opt < tcpheader:
            kind = payload[opt]
            if kind == 0: break
            if kind == 1: opt += 1; continue
            if opt + 1 >= tcpheader or payload[opt+1] < 2: break
            if kind == 3 and payload[opt+1] == 3: flow['window_scale'][direction] = payload[opt+2]
            opt += payload[opt+1]
    flow['bytes'][direction] += size
    if int.from_bytes(payload[14:16], 'big') == 0: flow['zero_windows'] += 1
    if size > 0:
        if (direction, seq, size) in flow['seen']: flow['retransmit_bytes'] += size
        flow['seen'].add((direction, seq, size))
        if direction in flow['last_data']: flow['max_data_gap'] = max(flow['max_data_gap'], now-flow['last_data'][direction])
        flow['last_data'][direction] = now
for flow in flows.values():
    flow['seconds'] = flow['last'] - flow['first']; del flow['seen']; del flow['last_data']
print(json.dumps(dict(dns=dns, flows=flows), indent=2))
