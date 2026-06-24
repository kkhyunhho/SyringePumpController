"""Unit tests for the USB port resolver (`_resolve_port`).

Pure logic — the attached-port list is mocked, so no hardware is needed.
Covers path pass-through, VID:PID matching, the optional serial component,
and the zero/several-match error paths.
"""

from __future__ import annotations

from dataclasses import dataclass

import pytest

import sy01b.syringe_pump_controller as drv
from sy01b.syringe_pump_controller import _resolve_port


@dataclass
class _FakePort:
    """Minimal stand-in for a serial.tools.list_ports ListPortInfo."""

    device: str
    vid: int | None
    pid: int | None
    serial_number: str | None


def _patch_ports(monkeypatch, ports: list[_FakePort]) -> None:
    monkeypatch.setattr(drv.serial.tools.list_ports, "comports", lambda: ports)


def test_explicit_unix_path_passthrough(monkeypatch):
    _patch_ports(monkeypatch, [])
    assert _resolve_port("/dev/ttyUSB0") == "/dev/ttyUSB0"


def test_explicit_com_path_passthrough(monkeypatch):
    _patch_ports(monkeypatch, [])
    assert _resolve_port("COM3") == "COM3"


def test_vid_pid_unique_match(monkeypatch):
    _patch_ports(
        monkeypatch,
        [
            _FakePort("/dev/ttyUSB0", 0x1A86, 0x7523, None),
            _FakePort("/dev/ttyUSB4", 0x110A, 0x1150, None),
        ],
    )
    assert _resolve_port("1A86:7523") == "/dev/ttyUSB0"


def test_vid_pid_no_match_raises(monkeypatch):
    _patch_ports(monkeypatch, [])
    with pytest.raises(RuntimeError, match="no serial device matches"):
        _resolve_port("1A86:7523")


def test_vid_pid_several_matches_raises(monkeypatch):
    _patch_ports(
        monkeypatch,
        [
            _FakePort("/dev/ttyUSB0", 0x0403, 0x6001, "AAAA"),
            _FakePort("/dev/ttyUSB1", 0x0403, 0x6001, "BBBB"),
        ],
    )
    with pytest.raises(RuntimeError, match="matches several devices"):
        _resolve_port("0403:6001")


def test_serial_pins_one_of_several(monkeypatch):
    _patch_ports(
        monkeypatch,
        [
            _FakePort("/dev/ttyUSB0", 0x0403, 0x6001, "AAAA"),
            _FakePort("/dev/ttyUSB1", 0x0403, 0x6001, "BBBB"),
        ],
    )
    assert _resolve_port("0403:6001:BBBB") == "/dev/ttyUSB1"


def test_bad_spec_raises_value_error(monkeypatch):
    _patch_ports(monkeypatch, [])
    with pytest.raises(ValueError, match="VID:PID"):
        _resolve_port("not-a-port")
