import sys
import socket
import tempfile
import unittest
from pathlib import Path
from unittest.mock import MagicMock, patch

sys.path.insert(0, str(Path(__file__).parents[1] / "tools"))
import vm_control


class ScreenshotTests(unittest.TestCase):
    def test_explicit_existing_capture_is_preserved_without_contacting_vm(self):
        with tempfile.TemporaryDirectory() as temporary:
            original = Path(temporary) / "evidence.png"
            original.write_bytes(b"previous capture")
            linked = Path(temporary) / "linked.png"
            linked.symlink_to(original)
            dangling = Path(temporary) / "dangling.png"
            dangling.symlink_to(Path(temporary) / "missing.png")
            for target in (original, linked, dangling):
                with self.subTest(target=target.name), \
                        patch.object(sys, "argv", ["vm_control.py", temporary, "screenshot", str(target)]), \
                        patch.object(vm_control, "request") as request, \
                        patch("sys.stderr"):
                    with self.assertRaises(SystemExit) as error:
                        vm_control.main()
                    self.assertEqual(error.exception.code, 1)
                    request.assert_not_called()
            self.assertEqual(original.read_bytes(), b"previous capture")

    def test_repeated_default_captures_do_not_overwrite_previous_evidence(self):
        with tempfile.TemporaryDirectory() as temporary:
            original = Path(temporary) / "desktop.png"
            original.write_bytes(b"previous capture")
            def capture(_directory, _command, arguments):
                with Path(arguments["filename"]).open("xb") as stream:
                    stream.write(b"new capture")
            with patch.object(sys, "argv", ["vm_control.py", temporary, "screenshot"]), \
                    patch.object(vm_control, "request", side_effect=capture), patch("builtins.print"):
                self.assertEqual(vm_control.main(), 0)
                self.assertEqual(vm_control.main(), 0)
            self.assertEqual(original.read_bytes(), b"previous capture")
            self.assertEqual(len(list(Path(temporary).glob("*.png"))), 3)


class Stream:
    def __init__(self, chunks):
        self.chunks = list(chunks)
        self.writes = []

    def write(self, data):
        self.writes.append(data)

    def flush(self):
        pass

    def close(self):
        pass

    def readline(self, _limit=None):
        return self.chunks.pop(0) if self.chunks else b""


class AgentProtocolTests(unittest.TestCase):
    def call(self, chunks):
        stream = Stream(chunks)
        connection = MagicMock()
        connection.__enter__.return_value = connection
        connection.makefile.return_value = stream
        with patch.object(vm_control.socket, "socket", return_value=connection), \
                patch.object(vm_control.uuid, "uuid4") as nonce:
            nonce.return_value.int = 42
            result = vm_control.agent_request("/tmp/virtual-machine", "guest-ping")
        return result, stream.writes

    def test_stale_replies_cannot_be_mistaken_for_current_result(self):
        result, writes = self.call([
            b'partial old data\xfe{"return":{"pid":123}}\n',
            b'\xff{"return":41}\n',
            b'{"return":42}\n',
            b'\xff{"return":42}\n',
            b'{"return":{"current":true}}\n',
        ])
        self.assertEqual(result, {"current": True})
        self.assertTrue(writes[0].startswith(b'\xff{"execute": "guest-sync-delimited"'))
        self.assertEqual(len(writes), 2)
        self.assertIn(b'"execute": "guest-ping"', writes[1])

    def test_split_delimited_reply_after_oversized_stale_frame(self):
        result, _ = self.call([
            b'\xff{"old":"' + b'x' * 65500,
            b'old frame remainder"}\n',
            b'\xff{"ret', b'urn":42}', b'\n', b'{"return":{}}\n',
        ])
        self.assertEqual(result, {})

    def test_wrong_nonce_and_disconnect_do_not_dispatch_command(self):
        stream = Stream([b'\xff{"return":41}\n'])
        connection = MagicMock()
        with patch.object(vm_control.uuid, "uuid4") as nonce:
            nonce.return_value.int = 42
            with self.assertRaisesRegex(ConnectionError, "synchronization"):
                vm_control.sync_agent(connection, stream)
        self.assertEqual(len(stream.writes), 1)

    def test_disconnect_after_sync_is_reported(self):
        with self.assertRaisesRegex(ConnectionError, "before replying"):
            self.call([b'\xff{"return":42}\n'])

    def test_command_error_is_not_retried(self):
        with self.assertRaisesRegex(ValueError, "CommandNotFound"):
            self.call([b'\xff{"return":42}\n', b'{"error":{"class":"CommandNotFound"}}\n'])

    def test_retained_error_traceback_does_not_hold_the_connection_open(self):
        for function, replies in (
            (vm_control.request, b'{"QMP":{}}\n{"return":{}}\n{"error":{"class":"CommandNotFound"}}\n'),
            (vm_control.agent_request, b'\xff{"return":42}\n{"error":{"class":"CommandNotFound"}}\n'),
        ):
            with self.subTest(function=function.__name__):
                client, server = socket.socketpair()
                self.addCleanup(client.close)
                self.addCleanup(server.close)
                server.settimeout(0.2)
                server.sendall(replies)
                connection = MagicMock()
                connection.__enter__.return_value = connection
                connection.__exit__.side_effect = lambda *_: client.close()
                connection.makefile.side_effect = client.makefile
                retained = []
                with patch.object(vm_control.socket, "socket", return_value=connection), \
                        patch.object(vm_control.uuid, "uuid4") as nonce:
                    nonce.return_value.int = 42
                    try:
                        function("/tmp/virtual-machine", "unsupported")
                    except ValueError as error:
                        retained.append(error)
                self.assertEqual(len(retained), 1)
                while server.recv(4096):
                    pass
