"""Bounded serial-driven QEMU harness. A timeout without markers always fails."""
import os
import selectors
import subprocess
import time


def boot(image, *, cpu="qemu64", memory="128M", terminal=b"initialization complete; halted", expected=(), timeout=15):
    cmd = ["qemu-system-x86_64", "-machine", "pc", "-cpu", cpu, "-m", memory,
           "-cdrom", str(image), "-display", "none", "-serial", "stdio", "-monitor", "none",
           "-no-reboot", "-no-shutdown", "-nic", "none"]
    output = bytearray()
    with subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT) as proc:
        try:
            with selectors.DefaultSelector() as selector:
                selector.register(proc.stdout, selectors.EVENT_READ)
                deadline = time.monotonic() + timeout
                while time.monotonic() < deadline and terminal not in output:
                    for key, _ in selector.select(min(0.25, max(0, deadline-time.monotonic()))):
                        chunk = os.read(key.fileobj.fileno(), 4096)
                        if not chunk:
                            break
                        output.extend(chunk)
                    if proc.poll() is not None:
                        break
        finally:
            proc.terminate()
            try:
                tail, _ = proc.communicate(timeout=2)
            except subprocess.TimeoutExpired:
                proc.kill()
                tail, _ = proc.communicate()
            output.extend(tail)
    text = output.decode(errors="replace")
    assert terminal in output, f"terminal marker not observed:\n{text}"
    for marker in expected:
        assert marker in output, f"missing {marker!r}:\n{text}"
    if b"fatal" not in terminal:
        assert b"JeiOS fatal:" not in output, text
    return text
