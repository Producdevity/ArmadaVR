#!/usr/bin/env python3
"""Serve metadata only in the isolated SSC VM; reject all stream commands."""
import ctypes
import json
from pathlib import Path
import runpy
import sys
import time

if (Path('/proc/device-tree/compatible').read_bytes().split(b'\0')[0] != b'linux,dummy-virt' or
        'reframed.ssc_test=1' not in Path('/proc/cmdline').read_text().split()):
    raise SystemExit('This mock requires the isolated SSC VM')
build = Path(sys.argv[1]).resolve()
sys.path[:0] = [str(build / 'mocking'), str(build / 'mocking/ssc_server')]
namespace = runpy.run_path(str(build / 'mocking/ssc_server/ssc-server'), run_name='ssc_mock')
ssc, common = namespace['SSC'], namespace['SscCommon']
from ssc_server import ssc_sensor_suid_pb2 as suid

fault = sys.argv[2]
assert fault in ('normal', 'empty', 'duplicate', 'zero', 'too-many', 'wrong-type', 'registry-empty',
                 'malformed-suid', 'malformed-attr', 'duplicate-attr', 'nan', 'missing-attr',
                 'wrong-uid', 'late-ack', 'no-ack', 'server-loss', 'error-ack')


class Server(namespace['SSCMockingServer']):
    def _reply_msg(self, node, port, incoming):
        if incoming[0] != namespace['QMI_REQUEST']:
            return
        _, data, transaction = ssc.parse_message_control_input(incoming)
        message = common.SscClientRequest()
        message.ParseFromString(data)
        assert message.msg_id in (512, 1), 'Collector attempted a stream command'
        print('SSC_INVENTORY_REQUEST', json.dumps({'id': message.msg_id,
              'high': str(message.uid.high), 'low': str(message.uid.low)}), flush=True)
        response = common.SscClientResponse()
        response.uid.CopyFrom(message.uid)
        event = response.response.add()
        event.timestamp = 0x123456789ABCDEF
        if message.msg_id == 512:
            assert message.uid.high == message.uid.low == 0xABABABABABABABAB
            request = suid.SscSuidRequest()
            request.ParseFromString(message.request.msg)
            assert request.HasField('enable_updates') and not request.enable_updates
            assert request.HasField('only_default_values') and not request.only_default_values
            reply = suid.SscSuidResponse()
            reply.data_type = request.data_type
            registry = request.data_type == 'registry'
            count = 1 if registry else 2
            if (registry and fault == 'registry-empty') or (not registry and fault == 'empty'):
                count = 0
            if not registry and fault == 'too-many':
                count = 65
            for i in range(count):
                uid = reply.uid.add()
                uid.high, uid.low = (0x1234, 0x5678) if registry else (0xACC10001 + i, 0xACC20001 + i)
            if not registry:
                if fault == 'duplicate':
                    reply.uid[1].CopyFrom(reply.uid[0])
                if fault == 'zero':
                    reply.uid[1].high = reply.uid[1].low = 0
                if fault == 'wrong-type':
                    reply.data_type = 'wrong'
            event.msg_id = 768
            event.msg = b'\xff' if not registry and fault == 'malformed-suid' else reply.SerializeToString()
        else:
            request = common.SscAttrRequest()
            request.ParseFromString(message.request.msg)
            assert request.HasField('enable_updates') and not request.enable_updates
            assert (message.uid.high, message.uid.low) in ((0xACC10001, 0xACC20001), (0xACC10002, 0xACC20002))
            reply = common.SscAttrResponse()
            for ident, values in [(0, ['candidate\n"é']), (1, ['fixture']), (3, [True]),
                                  (6, [5.0, 1000.0, 2000.0]), (16, [0]), (19, [0]),
                                  (20, [[1.0, 0.0, -1.0]]), (999, [2**64 - 1])]:
                attr = reply.attr.add()
                attr.id = ident
                for value in values:
                    out = attr.value_array.v.add()
                    if isinstance(value, bool):
                        out.b = value
                    elif isinstance(value, int):
                        out.i = value
                    elif isinstance(value, str):
                        out.s = value
                    elif isinstance(value, list):
                        for element in value:
                            out.a.element.add().f = element
                    else:
                        out.f = value
            if fault == 'duplicate-attr':
                reply.attr.add().CopyFrom(reply.attr[0])
            if fault == 'nan':
                reply.attr[3].value_array.v[0].f = float('nan')
            if fault == 'wrong-uid':
                response.uid.high ^= 1 << 63
            event.msg_id = 128
            event.msg = b'\xff' if fault == 'malformed-attr' else reply.SerializeToString()

        def send(raw):
            assert self._qrtr.qrtr_sendto(self.sock, node, port, ctypes.c_char_p(raw), len(raw)) == 0

        ack = ssc.generate_message_control_output(transaction)
        if fault == 'error-ack':
            # QMI result TLV carries uint16 failure=1 and error=1.
            assert ack[7:14] == b'\2\4\0\0\0\0\0'
            send(ack[:10] + b'\1\0\1\0' + ack[14:])
            return
        if fault == 'server-loss':
            raise SystemExit(0)
        if fault not in ('late-ack', 'no-ack'):
            send(ack)
        if fault != 'missing-attr' or message.msg_id != 1:
            send(ssc.generate_report_large_indication(response.SerializeToString(), transaction))
        if fault == 'late-ack':
            time.sleep(0.05)
            send(ack)


Server().start()
