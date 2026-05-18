"""Pure tests for the DT protocol layer — frame builder, parser, status decoder."""

from __future__ import annotations

import pytest

from sy01b import SyringePumpController

ETX = b"\x03"


class TestFormatAddress:
    @pytest.mark.parametrize(
        ("address", "expected"),
        [
            (1, b"1"),
            (2, b"2"),
            (9, b"9"),
            (10, b":"),
            (15, b"?"),
        ],
    )
    def test_valid_address(self, address: int, expected: bytes) -> None:
        assert SyringePumpController.format_address(address) == expected

    @pytest.mark.parametrize("bad", [0, -1, 16, 100])
    def test_out_of_range_raises(self, bad: int) -> None:
        with pytest.raises(ValueError, match="address"):
            SyringePumpController.format_address(bad)


class TestBuildCommand:
    def test_simple_query(self) -> None:
        assert SyringePumpController.build_command(1, "?23") == b"/1?23\r"

    def test_query_no_R_appended(self) -> None:
        # Read-only commands must never gain a trailing R.
        frame = SyringePumpController.build_command(1, "?23", execute=False)
        assert b"R" not in frame[3:-1]

    def test_execute_adds_R(self) -> None:
        assert SyringePumpController.build_command(1, "A100", execute=True) == b"/1A100R\r"

    def test_addr_two(self) -> None:
        assert SyringePumpController.build_command(2, "Q") == b"/2Q\r"

    def test_buffer_overflow_raises(self) -> None:
        with pytest.raises(ValueError, match="buffer"):
            SyringePumpController.build_command(1, "A" * 256)

    @pytest.mark.parametrize(
        ("cmds", "expected"),
        [
            ("I", b"/1IR\r"),
            ("O", b"/1OR\r"),
            ("I1", b"/1I1R\r"),
            ("I3", b"/1I3R\r"),
            ("O1", b"/1O1R\r"),
            ("O3", b"/1O3R\r"),
            ("w1,0", b"/1w1,0R\r"),
            ("w2,1", b"/1w2,1R\r"),
        ],
    )
    def test_valve_motion_frames(self, cmds: str, expected: bytes) -> None:
        assert SyringePumpController.build_command(1, cmds, execute=True) == expected


def _frame(status_byte: int, data: bytes = b"") -> bytes:
    return b"/0" + bytes([status_byte]) + data + ETX + b"\r\n"


class TestStatusByte:
    def test_ready_no_error(self) -> None:
        s = SyringePumpController.StatusByte.decode(0x40)
        assert s.busy is False
        assert s.error is SyringePumpController.ErrorCode.OK
        assert s.is_ok

    def test_busy_no_error(self) -> None:
        s = SyringePumpController.StatusByte.decode(0x60)
        assert s.busy is True
        assert s.error is SyringePumpController.ErrorCode.OK

    def test_ready_not_initialized(self) -> None:
        s = SyringePumpController.StatusByte.decode(0x47)
        assert s.busy is False
        assert s.error is SyringePumpController.ErrorCode.NOT_INITIALIZED
        assert not s.is_ok

    def test_busy_plunger_overload(self) -> None:
        s = SyringePumpController.StatusByte.decode(0x69)
        assert s.busy is True
        assert s.error is SyringePumpController.ErrorCode.PLUNGER_OVERLOAD

    def test_unknown_error_code_maps_to_unknown(self) -> None:
        s = SyringePumpController.StatusByte.decode(0x45)
        assert s.error is SyringePumpController.ErrorCode.UNKNOWN

    @pytest.mark.parametrize("bad", [0x00, 0x10, 0x80, 0xFF])
    def test_missing_fixed_frame_bit_raises(self, bad: int) -> None:
        with pytest.raises(SyringePumpController.ProtocolError, match="fixed bit-6"):
            SyringePumpController.StatusByte.decode(bad)


class TestParseReply:
    def test_status_only(self) -> None:
        reply = SyringePumpController.parse_reply(_frame(0x40))
        assert reply.status.error is SyringePumpController.ErrorCode.OK
        assert reply.data == b""

    def test_with_data(self) -> None:
        reply = SyringePumpController.parse_reply(_frame(0x40, b"V1.4"))
        assert reply.data == b"V1.4"
        assert reply.status.is_ok

    def test_data_may_contain_slash_zero(self) -> None:
        reply = SyringePumpController.parse_reply(_frame(0x40, b"/0/0"))
        assert reply.data == b"/0/0"

    @pytest.mark.parametrize(
        ("frame", "match"),
        [
            (b"", "too short"),
            (b"X0@\x03\r\n", "leading"),
            (b"/1@\x03\r\n", "master address"),
            (b"/0@no-etx-here\r\n", "ETX"),
        ],
    )
    def test_malformed_raises(self, frame: bytes, match: str) -> None:
        with pytest.raises(SyringePumpController.ProtocolError, match=match):
            SyringePumpController.parse_reply(frame)


class TestLeadingGarbageTolerance:
    """Regression guard for the CH340 stray-byte quirk discovered during HIL on 2026-05-15.

    The real EUSB-30 dongle on /dev/ttyUSB1 emitted ``\\xff/0`8.33\\x03\\r\\n`` for
    the first ``?23`` reply after open. The 0xFF is line/idle dribble from the CH340
    chip, not from the pump. The slicing logic in `SyringePumpController._send_and_receive` strips
    bytes before the leading '/'; `SyringePumpController.parse_reply` stays strict and rejects malformed
    frames. This test pins the slicing math so a refactor cannot regress it.
    """

    def test_buffer_slicing_strips_leading_garbage(self) -> None:
        buf = bytearray(b"\xff/0`8.33\x03\r\n")
        etx_index = buf.index(0x03)
        end = etx_index + 1
        while end < len(buf) and buf[end] in (0x0D, 0x0A):
            end += 1
        start = buf.find(b"/")
        cleaned = bytes(buf[start:end])
        assert cleaned == b"/0`8.33\x03\r\n"
        reply = SyringePumpController.parse_reply(cleaned)
        assert reply.data == b"8.33"
