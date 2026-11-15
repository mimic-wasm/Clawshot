"""Co-op (host/coop.c): a host and joiners as real processes over loopback UDP, with a fake microphone: the lobby, a joiner with
no key, a wrong key and the right one, Start, a new join refused after it, voice heard by the other player, and the distance
gain. Needs a C compiler and libopus's sources (third_party/opus), which it builds.

    python -I -B tests/test_coop.py [--cc clang]
"""
import argparse
import re
import subprocess
import sys
import tempfile
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
RUN_ALL_SKIP = {"windows": "POSIX sockets and processes: checked on Linux"}


def check(ok, what):
    if not ok:
        print("FAIL:", what)
        sys.exit(1)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--cc", default="clang")
    cc = parser.parse_args().cc
    opus = ROOT / "third_party" / "opus"
    with tempfile.TemporaryDirectory(prefix="coop-") as tmp:
        tmp = Path(tmp)
        flags = ["-O1", "-w", "-DOPUS_BUILD", "-DUSE_ALLOCA", "-DHAVE_LRINTF", "-DHAVE_LRINT"] + \
                ["-I" + str(opus / d) for d in ("include", "celt", "silk", "silk/float", "src")]
        procs = []
        objs = []
        for rel in (opus / "sources.txt").read_text().split():
            obj = tmp / (rel.replace("/", "_") + ".o")
            objs.append(str(obj))
            procs.append(subprocess.Popen([cc] + flags + ["-c", str(opus / rel), "-o", str(obj)]))
            if len(procs) >= 8:
                check(procs.pop(0).wait() == 0, "opus compiles")
        for p in procs:
            check(p.wait() == 0, "opus compiles")
        exe = tmp / "coop_test"
        subprocess.run([cc, "-std=gnu11", "-O1", "-Wall", "-Wextra", "-Wno-unused-parameter", "-I" + str(opus / "include"),
                        "-I" + str(ROOT / "host"), str(ROOT / "tests" / "coop_test.c"), str(ROOT / "host" / "coop.c")] + objs +
                       ["-lm", "-lpthread", "-o", str(exe)], check=True)

        def run(*args):
            return subprocess.Popen([str(exe), *args], stdout=subprocess.PIPE, text=True)

        # a session with no key: Bob joins, the host starts at 2 s, Late joins after that
        host = run("host", "Alice", "", "8")
        time.sleep(0.5)
        bob = run("client", "Bob", "", "6")
        time.sleep(4)
        late = run("client", "Late", "", "2")
        h, b, l = host.communicate()[0], bob.communicate()[0], late.communicate()[0]
        check("host-command=1" in h and "start=1" in h, "the host hosts and starts")
        check(re.search(r"tick mode=3 started=0 count=2 .*Alice p1=Bob\*", b), "Bob joined the lobby with Alice")
        check(re.search(r"tick mode=3 started=1", b), "Bob's session started when the host started it")
        check("voice-heard" in b and "voice-heard" in h, "each hears the other's voice, both ways")
        check("already started" in l and "mode=0" in l, "a join after Start is refused")
        check("gain 1.000 0.250 0.000" in h, "gain: full near, quarter at the middle, none at the range")
        # a key: none, wrong, right (all before the host starts at 2 s)
        host = run("host", "Alice", "secret", "5")
        time.sleep(0.4)
        nokey, wrong, good = run("client", "A", "", "1"), run("client", "B", "wrong", "1"), run("client", "C", "secret", "1")
        out = [p.communicate()[0] for p in (nokey, wrong, good)]
        host.communicate()
        check("needs a key" in out[0], "no key: told it needs one")
        check("Wrong key" in out[1], "a wrong key is refused")
        check("tick mode=3" in out[2], "the right key joins")
    print("PASS: co-op lobby, key, start lock and proximity voice")


if __name__ == "__main__":
    main()
