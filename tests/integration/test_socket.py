"""Tests to exercise socket-based read and write operations in the Tracker."""
import subprocess
import sys
import textwrap

import pytest

from bloomberg.pensieve._destination import SocketDestination
from bloomberg.pensieve._pensieve import AllocatorType
from bloomberg.pensieve._pensieve import MemoryAllocator
from bloomberg.pensieve._pensieve import SocketReader
from bloomberg.pensieve._pensieve import Tracker


class TestSocketWriter:
    def test_socket_writer(self, free_port):
        # GIVEN
        tracked_app = textwrap.dedent(
            f"""\
            from bloomberg.pensieve._pensieve import MemoryAllocator
            from bloomberg.pensieve._pensieve import SocketDestination
            from bloomberg.pensieve._pensieve import Tracker

            allocator = MemoryAllocator()
            with Tracker(destination=SocketDestination(port={free_port})):
                allocator.valloc(1234)
                allocator.free()
        """
        )

        proc = subprocess.Popen([sys.executable, "-c", tracked_app])

        # WHEN
        reader = SocketReader(port=free_port)

        # THEN
        records = list(reader.get_allocation_records())
        assert len(records) == 2
        alloc, free = records
        assert alloc.allocator == AllocatorType.VALLOC
        assert alloc.size == 1234
        symbol, file, line = alloc.stack_trace()[0]
        assert symbol == "valloc"
        assert file == "src/bloomberg/pensieve/_pensieve.pyx"
        assert line > 0 < 200
        assert free.allocator == AllocatorType.FREE
        assert free.size == 0
        assert proc.wait() == 0

    def test_tracker_handles_disconnection(self, free_port):
        """Check that we gracefully handle the case when a `SocketReader` disconnects before
        the tracking completes."""

        # GIVEN
        reader_app = textwrap.dedent(
            f"""
            from bloomberg.pensieve._pensieve import SocketReader
            from bloomberg.pensieve._pensieve import AllocatorType

            reader = SocketReader(port={free_port})
            record = next(reader.get_allocation_records())
            assert record.allocator == AllocatorType.VALLOC
            """
        )

        reader_process = subprocess.Popen([sys.executable, "-c", reader_app])

        allocator = MemoryAllocator()
        tracker = Tracker(destination=SocketDestination(port=free_port))

        # WHEN / THEN
        with tracker:
            allocator.valloc(1234)
            # At this point the reader will read the record and disconnect.
            assert reader_process.wait() == 0
            # From here on the tracker should be disabled.
            allocator.free()

        # And it stays disabled even when entering it again.
        with pytest.raises(RuntimeError, match="stale output"):
            with tracker:
                allocator.valloc(1234)
                allocator.free()


class TestSocketReader:
    def test_failure_to_connect(self, free_port):
        # GIVEN
        invalid_port = -1

        # WHEN
        with pytest.raises(Exception) as exc:
            SocketReader(invalid_port)

        # THEN
        assert exc.type is IOError
        assert exc.match("Failed to resolve host IP and port")