import importlib.util
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location('pico_imu', ROOT / 'tools/pico-imu.py')
IMU = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(IMU)


def capture():
    lines = ['PICO_IMU_CAPTURE 1',
             'SENSOR 35 ICM4x6xx direct=1 max_rate_level=3',
             'SENSOR 16 ICM4x6xx direct=1 max_rate_level=3',
             'TOKEN 35 351', 'TOKEN 16 161']
    for phase, counter in [('before', 10_000_000_000), ('after', 12_100_000_000)]:
        for clock, offset in [('monotonic', -3_000_000_000), ('boottime', -2_000_000_000)]:
            for _ in range(16):
                lines.append(f'CLOCK {phase} {clock} {counter + offset - 50} '
                             f'{counter * 192 // 10000} 19200000 {counter + offset + 50}')
    for i in range(12):
        for index, kind in enumerate((35, 16)):
            lines.append(f'SAMPLE {i * 2 + index + 1} {kind} {10_100_000_000 + i * 1_000_000} '
                         '1 2 3 146 0.003 0.001')
    return '\n'.join([*lines, 'COUNTS 12 12', 'PICO_IMU_CAPTURE_PASS']) + '\n'


class PicoImuCaptureTests(unittest.TestCase):
    def test_counter_mapping_and_raw_payload(self):
        report = IMU.analyze(capture())
        self.assertEqual(report['monotonic_offset_ns'], -3_000_000_000)
        self.assertEqual(report['exact_timestamp_pairs'], 12)
        self.assertEqual(report['mapping_uncertainty_ns'], 105)
        self.assertEqual(report['samples'][35][0]['monotonic_ns'], 7_100_000_000)
        self.assertEqual(report['samples'][16][0]['vendor_aux'], [146, 0.003, 0.001])
        self.assertEqual(report['streams']['16']['mean_rate_hz'], 1000)
        self.assertFalse(report['calibration_applied'])
        self.assertFalse(report['pose_estimated'])

    def test_rejects_partial_changed_or_ambiguous_stream(self):
        text = capture()
        changes = [
            text.replace('PICO_IMU_CAPTURE_PASS', ''),
            text.replace('PICO_IMU_CAPTURE 1', 'PICO_IMU_CAPTURE 2'),
            text.replace('COUNTS 12 12', 'COUNTS 13 12'),
            text.replace('SAMPLE 2 16', 'SAMPLE 3 16'),
            text.replace('SAMPLE 2 16', 'SAMPLE 2 4'),
            text.replace('10101000000', '10100000000'),
            text.replace(' 1 2 3 146', ' nan 2 3 146'),
            text.replace('TOKEN 16 161', 'TOKEN 16 351'),
            text.replace('TOKEN 16 161\n', ''),
            text.replace('TOKEN 16 161', 'TOKEN 16 161\nTOKEN 16 161'),
            text.replace('SENSOR 16 ICM4x6xx direct=1 max_rate_level=3\n', ''),
            text.replace('direct=1', 'direct=0'),
            text.replace('COUNTS 12 12', 'COUNTS 12 12\nCOUNTS 12 12'),
        ]
        for index, changed in enumerate(changes):
            with self.subTest(index=index), self.assertRaises(ValueError):
                IMU.analyze(changed)

    def test_rejects_bad_calibration_and_wrong_sample_domain(self):
        text = capture()
        lines = text.splitlines()
        for change in ['missing', 'frequency', 'wide', 'reset', 'jump', 'wrong-domain', 'future']:
            changed = []
            removed = False
            for line in lines:
                fields = line.split()
                if fields[0] == 'CLOCK':
                    if change == 'missing' and not removed:
                        removed = True
                        continue
                    if change == 'frequency' and fields[1] == 'after':
                        fields[5] = '24000000'
                    if change == 'wide':
                        fields[6] = str(int(fields[6]) + 3_000_000)
                    if change == 'reset' and fields[1] == 'after':
                        fields[4] = '1'
                    if change == 'jump' and fields[1] == 'after':
                        fields[3] = str(int(fields[3]) + 1_000_000)
                        fields[6] = str(int(fields[6]) + 1_000_000)
                if fields[0] == 'SAMPLE':
                    if change == 'wrong-domain':
                        fields[3] = str(int(fields[3]) - 3_000_000_000)
                    if change == 'future':
                        fields[3] = str(int(fields[3]) + 5_000_000_000)
                changed.append(' '.join(fields))
            with self.subTest(change=change), self.assertRaises(ValueError):
                IMU.analyze('\n'.join(changed))

    def test_sensor_service_must_be_idle(self):
        IMU.direct_connections('header\n  0 direct connections\nfooter')
        for text in ['10 direct connections', '1 direct connections', '',
                     '0 direct connections\n1 direct connections', 'Some text 0 direct connections']:
            with self.subTest(text=text), self.assertRaises(ValueError):
                IMU.direct_connections(text)


if __name__ == '__main__':
    unittest.main()
