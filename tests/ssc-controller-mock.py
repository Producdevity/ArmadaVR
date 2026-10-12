#!/usr/bin/env python3
"""Add Pico controller fixtures to the pinned upstream SSC mock server."""
import multiprocessing
from pathlib import Path
import re
import runpy
import sys
import time

build = Path(sys.argv[1]).resolve()
if (Path("/proc/device-tree/compatible").read_bytes().split(b"\0")[0] != b"linux,dummy-virt" or
        "armada.ssc_test=1" not in Path("/proc/cmdline").read_text().split()):
    raise SystemExit("This mock requires the isolated SSC test VM")
sys.path.insert(0, str(build / "mocking"))
sys.path.insert(0, str(build / "mocking/ssc_server"))
namespace = runpy.run_path(str(build / "mocking/ssc_server/ssc-server"), run_name="ssc_mock")
ssc = namespace["SSC"]
common = namespace["SscCommon"]
original_measurement = ssc.generate_protobuf_sensor_measurement
records = re.findall(r'\{1, 1, false,\s*"([a-f0-9]+)"\s*"([a-f0-9]+)",',
                     Path(sys.argv[2]).read_text())
assert len(records) == 2
records = [bytes.fromhex(a + b) for a, b in records]
assert all(len(record) == 100 for record in records)
fault = sys.argv[3] if len(sys.argv) == 4 else None
assert fault in (None, "late", "silent", "malformed")
data = ssc._read_data()
uids = {}
for index, name in enumerate(("ble_mcu", "not_ble_mcu")):
    high, low = 0xBEE10001 + index, 0xBEE20001 + index
    uids[(high, low)] = name
    data.append({"sensor": {"data_type": name, "uid_high": hex(high), "uid_low": hex(low)},
                 "attributes": {"name": {"id": 0, "value": name},
                                "vendor": {"id": 1, "value": "test"},
                                "available": {"id": 3, "value": True},
                                "sample_rate": {"id": 6, "value": 480.0},
                                "stream_type": {"id": 16, "value": 0}},
                 "measurements": []})
ssc._read_data = staticmethod(lambda: data)


def measurement(high, low, index):
    if (high, low) not in uids:
        return original_measurement(high, low, index)
    message = common.SscClientResponse()
    message.uid.high, message.uid.low = high, low
    event = message.response.add()
    event.msg_id = 1024
    event.timestamp = 0x123456789ABCDEF + (index + 1) * 1000
    event.msg = b"\x0a\x64" + records[index % 2] + b"\x10\x03"
    if index % 6 == 1:
        event.msg = b"\x10\x03"
    elif index % 6 == 2:
        message.uid.high ^= 0x8000000000000000
    elif index % 6 == 4:
        event.msg_id = 1234
    if fault == "malformed" or (fault == "silent" and index >= 6):
        event.msg = b"\x10\x03"
    return message.SerializeToString()


ssc.generate_protobuf_sensor_measurement = staticmethod(measurement)
multiprocessing.set_start_method("fork")
server = namespace["SSCMockingServer"]()
if fault == "late":
    original_reply = server._reply_msg

    def late_reply(node, port, incoming):
        if incoming[0] != namespace["QMI_REQUEST"]:
            return original_reply(node, port, incoming)
        _, data, _ = ssc.parse_message_control_input(incoming)
        message_id, _, _ = ssc.parse_protobuf_client_request(data)
        if message_id not in (namespace["SSC_PROTOBUF_ENABLE_REPORT_MSG_ID"],
                              namespace["SSC_PROTOBUF_ENABLE_CONTINUOUS_MSG_ID"]):
            return original_reply(node, port, incoming)
        send = server._qrtr.qrtr_sendto
        pending = []

        def defer(*args):
            pending.append(args)
            server._qrtr.qrtr_sendto = send
            return 0

        server._qrtr.qrtr_sendto = defer
        original_reply(node, port, incoming)
        assert len(pending) == 1
        time.sleep(1)
        send(*pending[0])

    server._reply_msg = late_reply
server.start()
