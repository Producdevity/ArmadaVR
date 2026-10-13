#!/usr/bin/env python3
"""Reject pre-rename runtime paths before assembling a new headset root."""
import argparse
from pathlib import Path


LEGACY_PATHS = (
    'opt/armada-vr', 'etc/armada-vr', 'usr/share/armada-vr',
    'usr/local/libexec/armada-vr', 'usr/share/armada-qcom-services',
    'usr/libexec/armada-*', 'usr/lib/systemd/system/armada-*',
    'etc/systemd/system/**/armada-*', 'etc/udev/rules.d/*-armada-vr-*',
)


def validate(root):
    root = Path(root)
    for pattern in LEGACY_PATHS:
        for path in root.glob(pattern):
            raise ValueError('Pre-rename userspace requires a fresh Reframed VR build: '
                             + str(path.relative_to(root)))


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('root', type=Path)
    args = parser.parse_args()
    try:
        validate(args.root)
    except (OSError, ValueError) as error:
        parser.exit(1, f'{error}\n')
