import os, pty, sys, signal, time, select
binpath, logpath, secs = sys.argv[1], sys.argv[2], float(sys.argv[3])
out = open(logpath, 'wb')
pid, fd = pty.fork()
if pid == 0:
    os.environ['VISUTWIN_BACKEND'] = 'vulkan'
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
