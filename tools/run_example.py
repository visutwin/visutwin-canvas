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

The run never outlasts the deadline by more than a few seconds, however much the
child logs: at the deadline it gets SIGTERM and GRACE seconds to exit, then
SIGKILL, and every read of what is left is capped in time. (An open-ended drain
that waited for the output to go quiet never ended on an example logging every
frame.)
"""
import os, pty, sys, signal, time, select

GRACE = 2.0   # seconds between SIGTERM and SIGKILL
TAIL = 1.0    # seconds to collect output already buffered once the child is gone

if len(sys.argv) != 4:
    sys.exit(__doc__)

binpath, logpath, secs = sys.argv[1], sys.argv[2], float(sys.argv[3])
out = open(logpath, 'wb')
pid, fd = pty.fork()
if pid == 0:
    os.execv(binpath, [binpath])


def pump(until, pid_to_reap=None):
    """Copy the child's output until `until` or EOF; return its exit status if it was reaped."""
    eof = False
    while time.time() < until:
        if eof:
            # The pty closed (the child is exiting); keep polling for its status.
            if pid_to_reap is None:
                break
            time.sleep(0.05)
        else:
            r, _, _ = select.select([fd], [], [], 0.2)
            if r:
                try:
                    data = os.read(fd, 65536)
                except OSError:
                    data = b''
                if data:
                    out.write(data); out.flush()
                else:
                    eof = True
        if pid_to_reap is not None:
            wpid, status = os.waitpid(pid_to_reap, os.WNOHANG)
            if wpid == pid_to_reap:
                return status
    return None


status = pump(time.time() + secs, pid)
if status is not None:
    pump(time.time() + TAIL)
    print(f"DIED({os.waitstatus_to_exitcode(status)})")
else:
    try:
        os.kill(pid, signal.SIGTERM)
    except ProcessLookupError:
        pass
    status = pump(time.time() + GRACE, pid)
    if status is None:
        try:
            os.kill(pid, signal.SIGKILL)
        except ProcessLookupError:
            pass
        # A killed child cannot finish exiting while its pty output is undrained (the
        # last close of a tty waits for the output to drain), so keep reading while
        # reaping; past TAIL, closing our end hangs the pty up and releases it.
        status = pump(time.time() + TAIL, pid)
        if status is None:
            os.close(fd)
            fd = -1
            os.waitpid(pid, 0)
    if fd >= 0:
        pump(time.time() + TAIL)
    print("ALIVE")
out.close()
