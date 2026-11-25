"""The app casting to a real player: sendspin-cpp-cli, reached on loopback by its URL.

The tests run in order against one player and share its state.
"""

import os
import signal
import subprocess
import time
from pathlib import Path

import pytest
from harness import APP, Process, free_port, kill_children, play_to_sendspin, private_env, wait_until, write_tone

CLI = os.environ.get("SENDSPIN_CLI", "sendspin-cli")
NAME = "Interop Speaker"
TICKED = r"\[x\] " + NAME
UNTICKED = r"\[ \] " + NAME



class NotStreaming(AssertionError):
    """The player answers, and says no stream has started."""


# Only the player staying silent is expected; anything else that goes wrong fails the test.
# Remove both marks when a server pin with the fix lands: they fail the run once the player plays.
SILENT_PLAYER = pytest.mark.xfail(
    strict=True,
    raises=NotStreaming,
    reason="sendspin-server-cpp sends stream/start before it declares playback, which a player on "
    "sendspin-cpp >= v0.10.0 ignores; fixed by Sendspin/sendspin-server-cpp#108",
)


class Cast:
    def __init__(self, home: Path):
        self.env = private_env(home)
        self.port = free_port()
        self.socket = str(home / "player.sock")
        self.settings = home / "data" / "sendspin" / "cast.ini"
        self.tone = write_tone(home / "tone.wav", 440, 4)
        self.app: Process | None = None

        self.player = Process(
            "the player",
            [CLI, "-n", NAME, "--port", str(self.port), "--state-dir", str(home / "player"),
             "--control-socket", self.socket, "--config", "/dev/null", "--no-mdns", "-o", "null",
             "--allow-unpaired", "-d", "info"],
            self.env,
        )  # fmt: skip
        self.player.wait_for(r"listening on port")

        # A speaker mDNS can't find, as there is no mDNS here
        self.settings.parent.mkdir(parents=True)
        self.settings.write_text(f"[General]\nspeaker_urls=ws://127.0.0.1:{self.port}/sendspin\n")

    def start_app(self, *args: str):
        self.app = Process("the app", [APP, "--list", *args], self.env)
        self.app.wait_for(r"serving on port")

    def stop_app(self):
        assert self.app.stop() == 0, self.app.tail()

    def cli(self, *args: str) -> str:
        """A subcommand, over the player's control socket."""
        done = subprocess.run(
            [CLI, *args, "--port", str(self.port), "--control-socket", self.socket],
            env=self.env, capture_output=True, text=True, timeout=10,
        )  # fmt: skip
        assert done.returncode == 0, f"sendspin-cli {' '.join(args)}: {done.stdout}{done.stderr}"
        return done.stdout

    def status(self) -> dict[str, str]:
        return dict(line.split("=", 1) for line in self.cli("status-kv").splitlines() if "=" in line)

    def expect_status(self, timeout: float = 10.0, **wanted: str):
        try:
            wait_until(lambda: wanted.items() <= self.status().items(), str(wanted), timeout)
        except AssertionError:
            status = self.status()
            got = {key: status.get(key) for key in wanted}
            raise AssertionError(
                f"the player never reported {wanted}; it reports {got} and ended with:\n{self.player.tail()}"
            ) from None


    def expect_streaming(self, timeout: float = 3.0):
        deadline = time.monotonic() + timeout
        while self.status()["streaming"] != "1":
            if time.monotonic() > deadline:
                raise NotStreaming(f"the player started no stream; it ended with:\n{self.player.tail()}")
            time.sleep(0.1)


@pytest.fixture(scope="module")
def cast(tmp_path_factory):
    try:
        cast = Cast(tmp_path_factory.mktemp("cast"))
        cast.start_app("--add", NAME)
        yield cast
        cast.stop_app()
        assert cast.player.stop(signal.SIGTERM) == 0, cast.player.tail()
    finally:
        kill_children()


@pytest.fixture
def tone(cast):
    playing = play_to_sendspin(cast.tone)
    try:
        yield playing
    finally:
        playing.kill()
        playing.wait()


def test_an_added_player_joins_the_group(cast):
    cast.app.wait_for(TICKED, 20)
    cast.expect_status(connected="1", trust="unpaired")


def test_the_groups_volume_and_mute_reach_the_player(cast):
    # The player's own controller asks; the app's server carries it out on its group
    cast.cli("vol", "30")
    cast.expect_status(group_volume="30", player_volume="30", player_volume_source="server")
    cast.cli("mute", "on")
    cast.expect_status(group_muted="1", player_muted="1")
    cast.cli("mute", "off")
    cast.expect_status(group_muted="0", player_muted="0")


def test_the_app_casts_what_plays_to_the_sendspin_output(cast, tone):
    cast.app.wait_for(r"Speakers \(playing\)", 5, since=cast.app.mark())


@SILENT_PLAYER
def test_the_player_gets_the_sound(cast, tone):
    cast.expect_streaming()


@SILENT_PLAYER
def test_the_sound_comes_back_after_an_idle_stop(cast):
    # 3 s of silence is idle: the stream ends, and the next sound starts another
    mark = cast.app.mark()
    cast.app.wait_for(r"^Speakers:", 10, since=mark)
    cast.expect_status(streaming="0")
    playing = play_to_sendspin(cast.tone)
    try:
        cast.expect_streaming()
    finally:
        playing.kill()
        playing.wait()


def test_a_restarted_app_casts_to_the_player_at_the_groups_volume(cast):
    cast.stop_app()
    assert "client_id=" in cast.settings.read_text(), "the added player was not remembered"
    # As the app keeps what its own slider and mute button set
    cast.settings.write_text(
        cast.settings.read_text().replace("[General]\n", "[General]\ngroup_volume=35\ngroup_muted=true\n")
    )
    cast.start_app()
    cast.app.wait_for(TICKED, 20)
    cast.expect_status(connected="1", player_volume="35", player_muted="1")


def test_a_removed_player_is_out_of_the_group(cast, tone):
    cast.stop_app()
    cast.start_app("--remove", NAME)
    cast.app.wait_for(UNTICKED, 20)
    mark = cast.app.mark()
    # The tone still plays, and nothing is added: the app casts to no one
    time.sleep(1.5)
    assert tone.poll() is None
    assert not cast.app.find(r"\(playing\)", since=mark), cast.app.tail()
    assert not cast.app.find(TICKED, since=mark), cast.app.tail()
    cast.expect_status(streaming="0")
    assert "client_id=" not in cast.settings.read_text()


def test_a_player_added_anew_gets_the_kept_volume_and_mute(cast):
    cast.stop_app()
    settings = cast.settings.read_text()
    assert "group_volume=35" in settings and "group_muted=true" in settings
    cast.settings.write_text(settings.replace("group_volume=35", "group_volume=45"))
    cast.start_app("--add", NAME)
    cast.app.wait_for(TICKED, 20)
    cast.expect_status(connected="1", player_volume="45", player_muted="1")


def test_this_pc_plays_along_beside_the_player(cast):
    # Its own output here is the private daemon's null sink
    cast.stop_app()
    # The timing line is debug
    cast.start_app("--add", NAME, "--play-along", "--verbose")
    cast.app.wait_for(TICKED, 20)
    mark = cast.app.mark()
    playing = play_to_sendspin(cast.tone)
    try:
        cast.app.wait_for(r"playing along on ", 5, since=mark)
        # In time with the group: on its timeline, not late for it
        off_us = int(cast.app.wait_for(r"this PC plays along: (-?\d+) us off", 5, since=mark).group(1))
        assert abs(off_us) < 1000, cast.app.tail()
        assert not cast.app.find(r"can't play along", since=mark), cast.app.tail()
    finally:
        playing.kill()
        playing.wait()
