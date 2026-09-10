"""Reference client for the VJVCPlus visualizer IPC outlet.

Connects to the named pipe served by `vjvcplus viz` and prints event
counts / samples. This is the protocol a future Unity (or any external
renderer) client must speak:

  * Pipe name : \\\\.\\pipe\\vjvcplus_viz  (byte stream, read-only)
  * Framing   : one JSON object per line (\\n-terminated), UTF-8
  * Messages  :
      {"type":"status","state":"standby|listening|matched|mixing"}
      {"type":"spectrum","bins":[64 x float 0..1],"peak":float}
      {"type":"track","valid":bool,"title":"...","artist":"...",
       "album":"...","cover_path":"local utf-8 path or empty",
       "confidence":float,"tentative":bool}

Usage:  python ipc_test_client.py
"""
import time

path = r"\\.\pipe\vjvcplus_viz"
deadline = time.time() + 25
with open(path, "r", encoding="utf-8", errors="replace") as f:
    print("CONNECTED")
    counts, samples = {}, []
    while time.time() < deadline:
        line = f.readline()
        if not line:
            break
        t = line.split('"type":"')[1].split('"')[0] if '"type":"' in line else "?"
        counts[t] = counts.get(t, 0) + 1
        if t in ("track", "status") and len(samples) < 8:
            samples.append(line.strip()[:160])
    print("COUNTS:", counts)
    for s in samples:
        print("SAMPLE:", s)
