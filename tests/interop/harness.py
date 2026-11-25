"""What both sides share: the processes under test, their output, and a tone to play."""

import math
import os
import re
import signal
import socket
import struct
import subprocess
import threading
import time
import wave
from pathlib import Path

APP = os.environ.get("SENDSPIN_APP", "build/src/sendspin-for-linux")
RATE = 48000

# A player prints its pairing token and codes; they stay out of what a failure quotes
SECRET = re.compile(r"SP:|PSK|[Pp]airing code")


def free_port() -> int:
    with socket.socket() as probe:
        probe.bind(("127.0.0.1", 0))
        return probe.getsockname()[1]


def private_env(home: Path) -> dict[str, str]:
    """The caller's environment with the app's data, settings and state under home."""
    return os.environ | {
        "XDG_DATA_HOME": str(home / "data"),
        "XDG_CONFIG_HOME": str(home / "config"),
        "XDG_STATE_HOME": str(home / "state"),
        "QT_QPA_PLATFORM": "offscreen",
        # Fedora's Qt logs to the journal when stderr is no terminal
        "QT_FORCE_STDERR_LOGGING": "1",
    }


def wait_until(check, what: str, timeout: float = 10.0):
    """Polls check() until it returns something true, which is returned."""
    deadline = time.monotonic() + timeout
    while True:
        value = check()
        if value:
            return value
        if time.monotonic() > deadline:
            raise AssertionError(f"timed out waiting for {what}")
        time.sleep(0.1)


def write_tone(path: Path, hz: int, seconds: float) -> Path:
    samples = (int(8000 * math.sin(2 * math.pi * hz * n / RATE)) for n in range(int(RATE * seconds)))
    with wave.open(str(path), "wb") as out:
        out.setnchannels(2)
        out.setsampwidth(2)
        out.setframerate(RATE)
        out.writeframes(b"".join(struct.pack("<hh", sample, sample) for sample in samples))
    return path


def play_to_sendspin(tone: Path) -> subprocess.Popen:
    """Plays a file into the app's Sendspin output, as a desktop app would."""
    return subprocess.Popen(["pw-play", "--target", "sendspin", str(tone)])


class Process:
    """A process whose output is kept line by line."""

    def __init__(self, name: str, argv: list[str], env: dict[str, str]):
        self.name = name
        self.lines: list[str] = []
        self._process = subprocess.Popen(
            argv, env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, errors="replace"
        )
        threading.Thread(target=self._read, daemon=True).start()

    def _read(self):
        for line in self._process.stdout:
            self.lines.append(line.rstrip("\n"))

    def mark(self) -> int:
        """Where the output ends now, for wait_for(since=)."""
        return len(self.lines)

    def find(self, pattern: str, since: int = 0) -> re.Match | None:
        for line in self.lines[since:]:
            if match := re.search(pattern, line):
                return match
        return None

    def wait_for(self, pattern: str, timeout: float = 10.0, since: int = 0) -> re.Match:
        try:
            return wait_until(lambda: self.find(pattern, since), pattern, timeout)
        except AssertionError:
            raise AssertionError(f"{self.name} never printed /{pattern}/; it ended with:\n{self.tail()}") from None

    def tail(self, count: int = 30) -> str:
        return "\n".join(line for line in self.lines[-count:] if not SECRET.search(line))

    def stop(self, how: int = signal.SIGINT) -> int:
        """Asks it to quit, as Ctrl-C does, and returns its exit code."""
        if self._process.poll() is None:
            self._process.send_signal(how)
        try:
            return self._process.wait(10)
        except subprocess.TimeoutExpired:
            self._process.kill()
            raise AssertionError(f"{self.name} did not quit; it ended with:\n{self.tail()}") from None
