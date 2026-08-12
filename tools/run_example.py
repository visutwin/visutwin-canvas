#!/usr/bin/env python3
"""Run an example under a pty for a fixed time, capturing all of its output.

spdlog block-buffers when stdout is a pipe or a file, so a run terminated by a
signal loses its tail -- exactly the part that matters. A pty keeps it
line-buffered.

    tools/run_example.py <binary> <logfile> <seconds>

The child inherits the caller's environment, so pick the backend and any
capture with env vars:

    VISUTWIN_BACKEND=vulkan          # or "metal" (lower case; case-sensitive)
    VISUTWIN_SCREENSHOT=out.png      # write a PNG of the backbuffer
    VISUTWIN_SCREENSHOT_FRAME=90     # which frame to capture (default 60)

Prints DIED(<code>) if the example exited on its own, ALIVE if it was still
running at the deadline and had to be killed.
"""
import os, pty, sys, signal, time, select

if len(sys.argv) != 4:
    sys.exit(__doc__)

binpath, logpath, secs = sys.argv[1], sys.argv[2], float(sys.argv[3])
out = open(logpath, 'wb')
pid, fd = pty.fork()
if pid == 0:
    os.execv(binpath, [binpath])
deadline = time.time() + secs
alive = True
while time.time() < deadline:
    r, _, _ = select.select([fd], [], [], 0.2)
    if r:
        try:
            data = os.read(fd, 65536)
        except OSError:
            break
        if not data: break
        out.write(data); out.flush()
    wpid, status = os.waitpid(pid, os.WNOHANG)
    if wpid == pid:
        alive = False
        code = os.waitstatus_to_exitcode(status)
        print(f"DIED({code})")
        break
if alive:
    # drain remaining
    while True:
        r, _, _ = select.select([fd], [], [], 0.3)
        if not r: break
        try: data = os.read(fd, 65536)
        except OSError: break
        if not data: break
        out.write(data); out.flush()
    os.kill(pid, signal.SIGKILL)
    os.waitpid(pid, 0)
    print("ALIVE")
out.close()
