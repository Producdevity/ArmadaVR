#!/usr/bin/env python3
"""Verify SSC report ownership, timestamps and object cleanup in pinned sources."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shlex
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path, help="Unmodified libssc source from profiles/ssc.json")
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--cc", default="gcc")
    args = parser.parse_args()
    profile = json.loads((ROOT / "profiles/ssc.json").read_text())
    source = args.source.resolve()
    for name, expected in profile["test_source_sha256"].items():
        if digest(source / name) != expected:
            raise ValueError("Source differs from the pinned libssc revision: " + name)
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    patched = output / "patched"
    shutil.copytree(source, patched)
    patches = [ROOT / name for name in profile["patches"]]
    with (output / "patch.log").open("w") as log:
        for patch in patches:
            if patch.name == '0005-own-pending-sensor-operations.patch':
                shutil.copytree(patched, output / 'operation-baseline')
            if patch.name == '0006-log-close-errors-before-transfer.patch':
                shutil.copytree(patched, output / 'close-baseline')
            if patch.name == '0007-correct-sensor-property-types.patch':
                shutil.copytree(patched, output / 'property-baseline')
            if patch.name == '0008-complete-calibration-sensor-startup.patch':
                shutil.copytree(patched, output / 'calibration-baseline')
            if patch.name == '0009-expose-rigid-body-identity.patch':
                shutil.copytree(patched, output / 'rigid-body-baseline')
            subprocess.run(["patch", "--batch", "--fuzz=0", "--forward", "-p1", "-i", str(patch)],
                           cwd=patched, stdout=log, stderr=subprocess.STDOUT, check=True, timeout=10)
    if digest(patched / "src/libssc-client.c") != profile["patched_report_source_sha256"]:
        raise ValueError("Patched dispatcher differs from the expected source")
    if digest(patched / "src/libssc-sensor.c") != profile["patched_sensor_source_sha256"]:
        raise ValueError("Patched sensor differs from the expected source")
    generated = output / "generated"
    generated.mkdir()
    subprocess.run(["protoc-c", "--proto_path=" + str(source / "data"),
                    "--c_out=" + str(generated), str(source / "data/ssc-common.proto"),
                    str(source / "data/ssc-sensor-suid.proto")],
                   check=True, timeout=10)
    flags = shlex.split(subprocess.check_output(
        ["pkg-config", "--cflags", "--libs", "gio-2.0", "qmi-glib", "qrtr-glib", "libprotobuf-c"], text=True))
    commands, lifetime, contexts = {}, {}, {}
    for name, tree in (("original", source), ("patched", patched)):
        binary = output / ("test-" + name)
        command = [args.cc, "-std=gnu11", "-g", "-O1", "-Wall", "-Wextra", "-Werror",
                   "-Wno-unused-parameter", "-fsanitize=address,undefined", '-DG_LOG_DOMAIN="libssc"']
        if name == "original":
            command.append("-DEXPECT_OLD")
        command += ["-I" + str(tree / "src"), "-I" + str(generated), "-I" + str(ROOT / "src"),
                    str(ROOT / "tests/ssc-reports.c"), str(tree / "src/libssc-common.c"),
                    str(generated / "ssc-common.pb-c.c"), str(ROOT / "src/pico-controller.c"),
                    *flags, "-o", str(binary)]
        with (output / ("build-" + name + ".log")).open("w") as log:
            subprocess.run(command, stdout=log, stderr=subprocess.STDOUT, check=True, timeout=60)
        env = dict(os.environ, SSC_TEST_WIRE_OUTPUT=str(output / ("wire-" + name + ".pb")),
                   ASAN_OPTIONS="detect_leaks=1", UBSAN_OPTIONS="halt_on_error=1")
        with (output / ("test-" + name + ".log")).open("w") as log:
            subprocess.run([str(binary)], env=env, stdout=log, stderr=subprocess.STDOUT,
                           check=True, timeout=15)
        commands[name] = command
        for kind in ("sensor", "client"):
            test = output / f"lifetime-{name}-{kind}"
            counterpart = "libssc-client.c" if kind == "sensor" else "libssc-sensor.c"
            command = [args.cc, "-std=gnu11", "-g", "-O1", "-Wall", "-Wextra", "-Werror",
                       "-Wno-unused-parameter", "-fsanitize=address,undefined", '-DG_LOG_DOMAIN="libssc"']
            if kind == "client":
                command.append("-DTEST_CLIENT")
            command += ["-I" + str(tree / "src"), "-I" + str(generated),
                        str(ROOT / "tests/ssc-lifetime.c"), str(tree / "src" / counterpart),
                        str(tree / "src/libssc-common.c"), str(generated / "ssc-common.pb-c.c"),
                        str(generated / "ssc-sensor-suid.pb-c.c"), *flags, "-o", str(test)]
            with (output / f"build-lifetime-{name}-{kind}.log").open("w") as log:
                subprocess.run(command, stdout=log, stderr=subprocess.STDOUT, check=True, timeout=60)
            result = subprocess.run([str(test)], env=env, capture_output=True, text=True, timeout=15)
            log = result.stdout + result.stderr
            (output / f"lifetime-{name}-{kind}.log").write_text(log)
            if name == "original":
                marker = "attempting double-free" if kind == "sensor" else "'freed' should be TRUE"
                if result.returncode == 0 or marker not in log:
                    raise ValueError("Original lifetime defect was not reproduced: " + kind)
            elif result.returncode != 0 or "SSC_LIFETIME_PASS" not in log:
                raise ValueError("Patched lifetime check failed: " + kind)
            lifetime[name + "-" + kind] = {"command": command, "returncode": result.returncode}
        test = output / ("context-" + name)
        command = [args.cc, "-std=gnu11", "-g", "-O1", "-Wall", "-Wextra", "-Werror",
                   "-Wno-unused-parameter", "-fsanitize=address,undefined", '-DG_LOG_DOMAIN="libssc"',
                   "-I" + str(tree / "src"), "-I" + str(generated), str(ROOT / "tests/ssc-context.c"),
                   str(tree / "src/libssc-common.c")]
        if name == "original":
            command.append("-DEXPECT_OLD")
        else:
            command += ["-DTEST_SENSOR", str(tree / "src/libssc-client.c"),
                        str(generated / "ssc-common.pb-c.c"), str(generated / "ssc-sensor-suid.pb-c.c")]
        command += [*flags, "-o", str(test)]
        with (output / ("build-context-" + name + ".log")).open("w") as log:
            subprocess.run(command, stdout=log, stderr=subprocess.STDOUT, check=True, timeout=60)
        result = subprocess.run([str(test)], env=env, capture_output=True, text=True, timeout=15)
        log = result.stdout + result.stderr
        (output / ("context-" + name + ".log")).write_text(log)
        if name == "original":
            if result.returncode == 0 or "SSC_CONTEXT_WRONG_DEFAULT_DISPATCH" not in log:
                raise ValueError("Original main-context defect was not reproduced")
        elif result.returncode != 0 or "SSC_CONTEXT_PASS" not in log:
            raise ValueError("Patched private-context test failed")
        contexts[name] = {"command": command, "returncode": result.returncode}
    if (output / "wire-original.pb").read_bytes() != (output / "wire-patched.pb").read_bytes():
        raise ValueError("The test inputs differ between source revisions")
    operations = {}
    for name, tree in (("baseline", output / 'operation-baseline'), ("patched", patched)):
        test = output / ('operation-test-' + name)
        command = [args.cc, '-std=gnu11', '-g', '-O1', '-Wall', '-Wextra', '-Werror',
                   '-Wno-unused-parameter', '-fsanitize=address,undefined', '-DG_LOG_DOMAIN="libssc"',
                   '-I' + str(tree / 'src'), '-I' + str(generated), str(ROOT / 'tests/ssc-operation.c'),
                   str(tree / 'src/libssc-client.c'), str(tree / 'src/libssc-common.c'),
                   str(generated / 'ssc-common.pb-c.c'), str(generated / 'ssc-sensor-suid.pb-c.c'),
                   *flags, '-o', str(test)]
        with (output / ('build-operation-' + name + '.log')).open('w') as log:
            subprocess.run(command, stdout=log, stderr=subprocess.STDOUT, check=True, timeout=60)
        cases = ['send-error', 'report-before-ack', 'cancel', 'timeout']
        if name == 'patched':
            cases += ['busy'] + [stage + '-' + case for stage in ('registry', 'suid', 'attribute')
                                for case in ('error', 'cancel', 'timeout')]
        results = {}
        for case in cases:
            result = subprocess.run([str(test), case], env=env, capture_output=True, text=True, timeout=15)
            log = result.stdout + result.stderr
            (output / ('operation-' + name + '-' + case + '.log')).write_text(log)
            if name == 'patched' or case == 'report-before-ack':
                if result.returncode or 'SSC_OPERATION_PASS ' + case not in log:
                    raise ValueError('SSC operation did not complete correctly: ' + name + '/' + case)
            else:
                marker = {'send-error': 'in report_received', 'cancel': '(completions == 1)',
                          'timeout': "'priv->discovery_timeout' should not be NULL"}[case]
                if not result.returncode or marker not in log:
                    raise ValueError('SSC operation defect not reproduced: ' + case)
            results[case] = result.returncode
        operations[name] = {'command': command, 'cases': results}
    close_results = {}
    for name, tree in (("baseline", output / 'close-baseline'), ("patched", patched)):
        test = output / ('close-test-' + name)
        command = [args.cc, '-std=gnu11', '-g', '-O1', '-Wall', '-Wextra', '-Werror',
                   '-Wno-unused-parameter', '-fsanitize=address,undefined', '-DG_LOG_DOMAIN="libssc"',
                   '-I' + str(tree / 'src'), '-I' + str(generated), str(ROOT / 'tests/ssc-close.c'),
                   str(tree / 'src/libssc-client.c'), str(tree / 'src/libssc-common.c'),
                   str(generated / 'ssc-common.pb-c.c'), str(generated / 'ssc-sensor-suid.pb-c.c'),
                   *flags, '-o', str(test)]
        with (output / ('build-close-' + name + '.log')).open('w') as log:
            subprocess.run(command, stdout=log, stderr=subprocess.STDOUT, check=True, timeout=60)
        result = subprocess.run([str(test)], env=env, capture_output=True, text=True, timeout=15)
        log = result.stdout + result.stderr
        (output / ('close-' + name + '.log')).write_text(log)
        if name == 'baseline':
            if not result.returncode or 'heap-use-after-free' not in log or 'in sensor_close_ready' not in log:
                raise ValueError('Close error ownership defect not reproduced')
        elif result.returncode or 'SSC_CLOSE_PASS cycles=100' not in log:
            raise ValueError('Close error ownership regression failed')
        close_results[name] = {'command': command, 'returncode': result.returncode}
    properties = {}
    for name, tree in (("baseline", output / 'property-baseline'), ("patched", patched)):
        test = output / ('property-test-' + name)
        command = [args.cc, '-std=gnu11', '-g', '-O1', '-Wall', '-Wextra', '-Werror',
                   '-Wno-unused-parameter', '-fsanitize=address,undefined', '-DG_LOG_DOMAIN="libssc"',
                   *(['-DEXPECT_OLD'] if name == 'baseline' else []),
                   '-I' + str(tree / 'src'), '-I' + str(generated), str(ROOT / 'tests/ssc-properties.c'),
                   str(tree / 'src/libssc-client.c'), str(tree / 'src/libssc-common.c'),
                   str(generated / 'ssc-common.pb-c.c'), str(generated / 'ssc-sensor-suid.pb-c.c'),
                   *flags, '-o', str(test)]
        with (output / ('build-property-' + name + '.log')).open('w') as log:
            subprocess.run(command, stdout=log, stderr=subprocess.STDOUT, check=True, timeout=60)
        result = subprocess.run([str(test)], env=env, capture_output=True, text=True, timeout=15)
        log = result.stdout + result.stderr
        (output / ('property-' + name + '.log')).write_text(log)
        marker = 'SSC_PROPERTIES_BASELINE_FAILURES 3' if name == 'baseline' else 'SSC_PROPERTIES_PASS cycles=1024'
        if result.returncode or marker not in log:
            raise ValueError('SSC sensor property comparison failed: ' + name)
        properties[name] = {'command': command, 'returncode': result.returncode}
    calibration = {}
    for name, tree in (("baseline", output / 'calibration-baseline'), ("patched", patched)):
        test = output / ('calibration-test-' + name)
        command = [args.cc, '-std=gnu11', '-g', '-O1', '-Wall', '-Wextra', '-Werror',
                   '-Wno-unused-parameter', '-fsanitize=address,undefined', '-DG_LOG_DOMAIN="libssc"',
                   *(['-DEXPECT_OLD'] if name == 'baseline' else []),
                   '-I' + str(tree / 'src'), '-I' + str(generated), str(ROOT / 'tests/ssc-calibration-open.c'),
                   str(tree / 'src/libssc-client.c'), str(tree / 'src/libssc-common.c'),
                   str(generated / 'ssc-common.pb-c.c'), str(generated / 'ssc-sensor-suid.pb-c.c'),
                   *flags, '-o', str(test)]
        with (output / ('build-calibration-' + name + '.log')).open('w') as log:
            subprocess.run(command, stdout=log, stderr=subprocess.STDOUT, check=True, timeout=60)
        result = subprocess.run([str(test)], env=env, capture_output=True, text=True, timeout=15)
        log = result.stdout + result.stderr
        (output / ('calibration-' + name + '.log')).write_text(log)
        marker = 'SSC_CALIBRATION_OPEN_BASELINE_PENDING 256' if name == 'baseline' else 'SSC_CALIBRATION_OPEN_PASS cycles=384'
        if result.returncode or marker not in log:
            raise ValueError('SSC calibration startup comparison failed: ' + name)
        calibration[name] = {'command': command, 'returncode': result.returncode}
    rigid_body = {}
    for name, tree in (("baseline", output / 'rigid-body-baseline'), ("patched", patched)):
        test = output / ('rigid-body-test-' + name)
        command = [args.cc, '-std=gnu11', '-g', '-O1', '-Wall', '-Wextra', '-Werror',
                   '-Wno-unused-parameter', '-fsanitize=address,undefined', '-DG_LOG_DOMAIN="libssc"',
                   *(['-DEXPECT_OLD'] if name == 'baseline' else []),
                   '-I' + str(tree / 'src'), '-I' + str(generated), str(ROOT / 'tests/ssc-rigid-body.c'),
                   str(tree / 'src/libssc-client.c'), str(tree / 'src/libssc-common.c'),
                   str(generated / 'ssc-common.pb-c.c'), str(generated / 'ssc-sensor-suid.pb-c.c'),
                   *flags, '-o', str(test)]
        with (output / ('build-rigid-body-' + name + '.log')).open('w') as log:
            subprocess.run(command, stdout=log, stderr=subprocess.STDOUT, check=True, timeout=60)
        result = subprocess.run([str(test)], env=env, capture_output=True, text=True, timeout=15)
        log = result.stdout + result.stderr
        (output / ('rigid-body-' + name + '.log')).write_text(log)
        marker = 'SSC_RIGID_BODY_BASELINE_MISSING' if name == 'baseline' else 'SSC_RIGID_BODY_PASS cycles=128'
        if result.returncode or marker not in log:
            raise ValueError('SSC rigid-body metadata comparison failed: ' + name)
        rigid_body[name] = {'command': command, 'returncode': result.returncode}
    report = {"passed": True, "source_commit": profile["commit"],
              "patches_sha256": {str(p.relative_to(ROOT)): digest(p) for p in patches},
              "test_sha256": digest(ROOT / "tests/ssc-reports.c"),
              "lifetime_test_sha256": digest(ROOT / "tests/ssc-lifetime.c"), "lifetime": lifetime,
              "context_test_sha256": digest(ROOT / "tests/ssc-context.c"), "contexts": contexts,
              "close_test_sha256": digest(ROOT / "tests/ssc-close.c"), "close_results": close_results,
              "property_test_sha256": digest(ROOT / "tests/ssc-properties.c"), "properties": properties,
              "calibration_test_sha256": digest(ROOT / "tests/ssc-calibration-open.c"), "calibration": calibration,
              "rigid_body_test_sha256": digest(ROOT / "tests/ssc-rigid-body.c"), "rigid_body": rigid_body,
              "operation_test_sha256": digest(ROOT / "tests/ssc-operation.c"), "operations": operations,
              "wire_sha256": digest(output / "wire-patched.pb"), "compile_commands": commands,
              "scope": "Actual protobuf dispatcher; no QRTR, DSP, radio or physical stream acceptance"}
    (output / "result.json").write_text(json.dumps(report, indent=2) + "\n")
    print((output / "test-patched.log").read_text(), end="")


if __name__ == "__main__":
    main()
