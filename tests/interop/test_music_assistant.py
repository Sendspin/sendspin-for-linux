"""The app as Music Assistant sees it: a server scripted on aiosendspin dials the app's client,
pairs by the code the PC shows, takes the PC as a source and plays to it.

The tests run in order on one connection and share its state.
"""

import asyncio
import inspect
import math
import re
import signal
import struct
import wave
from pathlib import Path

import numpy
import pytest
from aiosendspin.models.types import AudioCodec, PairingCodeFormat, PairMethod
from aiosendspin.noise.keys import Identity
from aiosendspin.noise.pairing import PairingAttempt
from aiosendspin.noise.trust_store import InMemoryServerPairingStore
from aiosendspin.server import AudioFormat, ClientAddedEvent, SendspinServer, SourceStreamStartedEvent
from aiosendspin.server.roles.metadata import Metadata
from dbus_fast import Message, MessageType, RequestNameReply
from dbus_fast.aio import MessageBus
from dbus_fast.service import ServiceInterface, method
from harness import APP, RATE, Process, kill_children, play_to_sendspin, private_env, write_tone

MPRIS = "org.mpris.MediaPlayer2.com.sendspin_audio.Sendspin"
SOURCE_HZ = 440
PLAYER_HZ = 1000
# ci/pipewire: where the app's player plays, there being no other output
SINK = "ci-null-sink"


class Notifications(ServiceInterface):
    """The desktop's notifications, which is where the app shows its pairing code."""

    def __init__(self):
        super().__init__("org.freedesktop.Notifications")
        self.summaries: list[str] = []

    @method()
    def Notify(self, app_name: "s", replaces_id: "u", app_icon: "s", summary: "s", body: "s",
               actions: "as", hints: "a{sv}", expire_timeout: "i") -> "u":  # fmt: skip
        self.summaries.append(summary)
        return replaces_id or 1

    @method()
    def CloseNotification(self, id: "u"):
        pass


def dominant_hz(pcm: bytes, bit_depth: int) -> float:
    """The loudest frequency in the left channel of interleaved stereo; 0 for silence."""
    width = bit_depth // 8
    frames = numpy.frombuffer(pcm, numpy.uint8).reshape(-1, 2 * width)[:, :width]
    padded = numpy.zeros((len(frames), 4), numpy.uint8)
    padded[:, 4 - width :] = frames
    left = padded.view("<i4").ravel().astype(numpy.float64)
    if not left.any():
        return 0.0
    spectrum = numpy.abs(numpy.fft.rfft(left * numpy.hanning(len(left))))
    return float(numpy.argmax(spectrum[1:]) + 1) * RATE / len(left)


class MusicAssistant:
    def __init__(self, home: Path):
        self.home = home
        self.loop = asyncio.new_event_loop()
        self.app: Process | None = None
        self.playing: asyncio.Task | None = None

    def run(self, work, timeout: float = 30.0):
        return self.loop.run_until_complete(asyncio.wait_for(work, timeout))

    async def until(self, check, what: str, timeout: float = 10.0):
        try:
            async with asyncio.timeout(timeout):
                while not (value := await check() if inspect.iscoroutinefunction(check) else check()):
                    await asyncio.sleep(0.1)
        except TimeoutError:
            raise AssertionError(f"timed out waiting for {what}; the app ended with:\n{self.app.tail()}") from None
        return value

    async def start(self):
        self.bus = await MessageBus().connect()
        self.notifications = Notifications()
        self.bus.export("/org/freedesktop/Notifications", self.notifications)
        # Ours alone, or the code would go to a real notification daemon
        owned = await self.bus.request_name("org.freedesktop.Notifications")
        assert owned == RequestNameReply.PRIMARY_OWNER, "another notification service is on this bus"

        self.app = Process("the app", [APP], private_env(self.home))
        port = int(self.app.wait_for(r"sendspin\.client: listening on port (\d+)").group(1))

        # No start_server(): nothing listens and nothing is advertised; the server only dials
        self.server = SendspinServer(
            self.loop, Identity.generate(), "Interop Server", pairing_store=InMemoryServerPairingStore()
        )
        added = self.loop.create_future()
        self.server.add_event_listener(
            lambda _, event: isinstance(event, ClientAddedEvent) and not added.done() and added.set_result(event.client_id)
        )
        await self.server.connect_to_client_and_wait(f"ws://127.0.0.1:{port}/sendspin", retry_initial_connection=True)
        self.client = self.server.get_client(await added)

    async def stop(self):
        if self.playing:
            self.playing.cancel()
        await self.server.close()
        self.bus.disconnect()

    async def shown_code(self) -> str:
        """The code as the person at the PC reads it. Never printed: it is a secret while it lasts."""
        shown = lambda: next(  # noqa: E731
            (m for s in reversed(self.notifications.summaries) if (m := re.search(r"Pairing code: ([\d-]+)", s))), None
        )
        return (await self.until(shown, "the pairing code's notification")).group(1)

    async def mpris(self) -> dict:
        """The player's MPRIS properties; empty while it is not on the bus."""
        reply = await self.bus.call(
            Message(
                destination=MPRIS,
                path="/org/mpris/MediaPlayer2",
                interface="org.freedesktop.DBus.Properties",
                member="GetAll",
                signature="s",
                body=["org.mpris.MediaPlayer2.Player"],
            )
        )
        if reply.message_type != MessageType.METHOD_RETURN:
            return {}
        return {name: value.value for name, value in reply.body[0].items()}

    async def play_tone(self):
        """Plays a tone to the PC until cancelled."""
        stream = self.client.group.start_stream()
        tone = b"".join(
            struct.pack("<hh", sample, sample)
            for sample in (int(8000 * math.sin(2 * math.pi * PLAYER_HZ * n / RATE)) for n in range(RATE // 10))
        )
        try:
            while True:
                stream.prepare_audio(tone, AudioFormat(RATE, 16, 2))
                await stream.commit_audio()
                await stream.sleep_to_limit_buffer(max_buffer_us=1_000_000)
        finally:
            stream.stop()

    async def record_sink(self, seconds: float) -> bytes:
        """What the sink the app plays on was given, as 16-bit stereo."""
        path = self.home / "sink.wav"
        recorder = await asyncio.create_subprocess_exec(
            "pw-record", "--target", SINK, "-P", "stream.capture.sink=true",
            "--rate", str(RATE), "--channels", "2", "--format", "s16", str(path),
        )  # fmt: skip
        await asyncio.sleep(seconds)
        recorder.send_signal(signal.SIGINT)
        await recorder.wait()
        with wave.open(str(path), "rb") as recorded:
            return recorded.readframes(recorded.getnframes())


@pytest.fixture(scope="module")
def ma(tmp_path_factory):
    ma = MusicAssistant(tmp_path_factory.mktemp("music-assistant"))
    try:
        ma.run(ma.start())
        yield ma
        ma.run(ma.stop())
        ma.app.stop()
    finally:
        kill_children()
        ma.loop.close()


def test_the_server_pairs_by_the_code_the_pc_shows(ma):
    async def pair():
        assert not ma.client.is_paired
        await ma.server.initiate_pairing(
            ma.client.client_id,
            PairingAttempt(
                method=PairMethod.DYNAMIC_PAIRING_CODE,
                pairing_code_provider=ma.shown_code,
                pairing_format=PairingCodeFormat.DIGITS,
            ),
        )
        assert ma.client.is_paired
        # The source is only for a paired server
        await ma.until(
            lambda: {"source@v1", "player@v1", "metadata@v1"} <= set(ma.client.active_role_ids),
            "the paired roles",
        )
        # With the server's name, when the app has it by then
        await ma.until(
            lambda: any(summary.startswith("Paired") for summary in ma.notifications.summaries),
            "the PC to say it paired",
        )

    ma.run(pair())


def test_the_pc_is_a_source_of_flac_with_sane_timestamps(ma):
    async def take_source():
        role = ma.client.role("source@v1")
        # The stream handle is decoded PCM; what came over the wire is in its start message
        codecs = []
        on_start = role.on_client_stream_start
        role.on_client_stream_start = lambda payload: (codecs.append(payload.source.codec), on_start(payload))[1]
        started = ma.loop.create_future()
        ma.client.add_event_listener(
            lambda _, event: isinstance(event, SourceStreamStartedEvent) and not started.done() and started.set_result(event)
        )

        playing = play_to_sendspin(write_tone(ma.home / "source.wav", SOURCE_HZ, 6))
        try:
            role.request_start()
            stream = await asyncio.wait_for(started, 10)
            chunks = []
            async with asyncio.timeout(10):
                async for pcm, timestamp_us in stream.handle:
                    chunks.append((pcm, timestamp_us))
                    if len(chunks) == 40:
                        break
            now_us = ma.server.clock.now_us()
        finally:
            playing.kill()
            playing.wait()
            role.request_stop()

        assert codecs == [AudioCodec.FLAC]
        audio = stream.audio_format
        assert (audio.sample_rate, audio.channels) == (RATE, 2)
        frame = audio.bit_depth // 8 * audio.channels

        stamps = [timestamp_us for _, timestamp_us in chunks]
        assert stamps == sorted(set(stamps)), "timestamps go forward"
        # Each chunk starts where the one before it ended
        for (pcm, at_us), next_us in zip(chunks, stamps[1:]):
            assert abs(next_us - at_us - len(pcm) // frame * 1_000_000 // RATE) < 2_000
        # On the server's clock, and of just now: when the output would have played it
        assert abs(now_us - stamps[-1]) < 1_000_000

        heard = dominant_hz(b"".join(pcm for pcm, _ in chunks[10:]), audio.bit_depth)
        assert abs(heard - SOURCE_HZ) < 10, f"the source carried {heard:.0f} Hz"

    ma.run(take_source())


def test_the_pc_plays_what_the_server_sends(ma):
    async def play():
        player = ma.client.role("player@v1")
        assert player.set_preferred_format(None, AudioCodec.FLAC)
        # As the server's slider does. A player nobody has set yet is at 0.
        player.set_volume(100)
        await ma.until(lambda: player.volume == 100, "the player to take its volume")
        ma.client.group.group_role("metadata").set_metadata(
            Metadata(title="Interop Title", artist="Interop Artist", album="Interop Album")
        )
        ma.playing = ma.loop.create_task(ma.play_tone())
        await ma.until(lambda: ma.app.find(r"sendspin\.playback: playing on"), "the app's PipeWire stream")
        # Past the start of the stream, whose first buffer is silence until its time comes
        await asyncio.sleep(1.5)
        heard = dominant_hz(await ma.record_sink(1.5), 16)
        assert abs(heard - PLAYER_HZ) < 10, f"the sink got {heard:.0f} Hz; the app ended with:\n{ma.app.tail()}"

    ma.run(play())


def test_mpris_shows_what_plays(ma):
    async def shown():
        async def playing():
            properties = await ma.mpris()
            return properties if properties.get("PlaybackStatus") == "Playing" else None

        properties = await ma.until(playing, "MPRIS to say it plays")
        metadata = {name: value.value for name, value in properties["Metadata"].items()}
        assert metadata["xesam:title"] == "Interop Title"
        assert metadata["xesam:artist"] == ["Interop Artist"]
        assert metadata["xesam:album"] == "Interop Album"

        ma.playing.cancel()
        await ma.client.group.stop()

        async def stopped():
            return (await ma.mpris()).get("PlaybackStatus") != "Playing"

        await ma.until(stopped, "MPRIS to say it stopped")

    ma.run(shown())
