"""Load test for PocketChat.

Opens N virtual users over WebSocket, lets the first one send ROUNDS messages
and measures how long each message takes to reach every user.
All bots run on one machine and one clock, so the difference between send time
and receive time is the fan-out latency.

Usage:
    pip install websockets
    python load_test.py 5          # 5 bots

Connect your computer to the board's WiFi first. If the chat has a different
address or port, change URL below.
"""
import asyncio
import json
import sys
import time

import websockets

URL = "ws://192.168.4.1:81/"
N = int(sys.argv[1]) if len(sys.argv) > 1 else 5   # number of virtual users
ROUNDS = 20                                          # number of test messages
RUN = int(time.time())                               # id of this run (to tell it apart from chat history)


async def main():
    conns = []
    for i in range(N):
        ws = await websockets.connect(URL)
        await ws.send(json.dumps({"type": "join", "name": f"bot{i}-{RUN % 1000}"}))
        conns.append(ws)
        await asyncio.sleep(0.3)
    print(f"{N} bots connected")

    sent, recv = {}, {}

    async def reader(i, ws):
        async for raw in ws:
            m = json.loads(raw)
            if m.get("type") == "msg" and m["text"].startswith(f"T:{RUN}:"):
                r = int(m["text"].split(":")[2])
                recv[(r, i)] = time.perf_counter()

    tasks = [asyncio.create_task(reader(i, ws)) for i, ws in enumerate(conns)]
    await asyncio.sleep(1)

    for r in range(ROUNDS):
        sent[r] = time.perf_counter()
        await conns[0].send(json.dumps({"type": "msg", "text": f"T:{RUN}:{r}"}))
        await asyncio.sleep(0.5)
    await asyncio.sleep(1.5)

    delays = [(recv[(r, i)] - sent[r]) * 1000
              for r in range(ROUNDS) for i in range(N) if (r, i) in recv]
    expected = N * ROUNDS
    delays.sort()
    print(f"received: {len(delays)}/{expected}  (lost: {expected - len(delays)})")
    if delays:
        print(f"latency ms -> avg: {sum(delays)/len(delays):.1f} | "
              f"median: {delays[len(delays)//2]:.1f} | max: {delays[-1]:.1f}")

    for t in tasks:
        t.cancel()
    for ws in conns:
        await ws.close()


asyncio.run(main())
