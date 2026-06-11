"""Endpoint tests against FakePump.

Covers: happy paths for every endpoint, the prime sequence mirrors
claude_test/prime_line.py, status-code mapping for representative
driver exceptions, no-traceback in error bodies.
"""

from __future__ import annotations

from typing import Any

from fastapi.testclient import TestClient

from sy01b import SyringePumpController, __version__

# FakePump is auto-injected by pytest via tests/server/conftest.py.
# We type its fixture argument as Any to avoid coupling test files to the
# conftest module path (tests/ is not a package; see DESIGN.md §14).
FakePump = Any


# ---------------------------------------------------------------- /v1/health
class TestHealth:
    def test_reports_pump_open_and_driver_version(
        self, client: TestClient
    ) -> None:
        body = client.get("/v1/health").json()
        assert body["pump_open"] is True
        assert body["driver_version"] == __version__
        assert body["diagnose_ok"] is None  # not run yet

    def test_diagnose_ok_set_after_diagnose(self, client: TestClient) -> None:
        client.get("/v1/diagnose")
        body = client.get("/v1/health").json()
        assert body["diagnose_ok"] is True


# -------------------------------------------------------------- /v1/diagnose
class TestDiagnose:
    def test_returns_report_fields(self, client: TestClient) -> None:
        body = client.get("/v1/diagnose").json()
        assert body["software_version"] == "FAKE-8.33"
        assert body["serial_number"] == "FAKE-SN-0001"
        assert body["supply_volts"] == 24.0
        assert body["ok_to_initialize"] is True
        assert body["pre_init_error_name"] == "NOT_INITIALIZED"

    def test_low_supply_voltage_returns_503(
        self, client: TestClient, fake_pump: FakePump
    ) -> None:
        fake_pump.inject_error(
            SyringePumpController.LowSupplyVoltageError(
                measured_v=18.0, min_v=22.0
            )
        )
        r = client.get("/v1/diagnose")
        assert r.status_code == 503
        body = r.json()
        assert body["error"] == "LowSupplyVoltageError"
        assert "18.0" in body["message"]


# ------------------------------------------------------------ /v1/initialize
class TestInitialize:
    def test_calls_driver_with_force_and_ccw(
        self, client: TestClient, fake_pump: FakePump
    ) -> None:
        r = client.post("/v1/initialize", json={"force": 2, "ccw": False})
        assert r.status_code == 200
        body = r.json()
        assert body["valve"] == "1"
        assert body["plunger_steps"] == 0
        assert ("initialize", (), {"force": 2, "ccw": False}) in (
            fake_pump.calls
        )

    def test_default_force_is_2(
        self, client: TestClient, fake_pump: FakePump
    ) -> None:
        client.post("/v1/initialize", json={})
        _, _, kw = next(c for c in fake_pump.calls if c[0] == "initialize")
        assert kw == {"force": 2, "ccw": False}

    def test_init_failed_returns_500(
        self, client: TestClient, fake_pump: FakePump
    ) -> None:
        fake_pump.inject_error(
            SyringePumpController.InitFailedError(
                error_code=SyringePumpController.ErrorCode.INIT_FAILED,
                command_sent="Z2",
                raw_reply=b"/0A\x03",
            )
        )
        r = client.post("/v1/initialize", json={"force": 2})
        assert r.status_code == 500
        body = r.json()
        assert body["error"] == "InitFailedError"
        assert body["code"] == 1
        assert body["command"] == "Z2"
        assert body["raw_reply_hex"] == "2f3041 03".replace(" ", "")


# ----------------------------------------------------------------- /v1/valve
class TestValve:
    def test_moves_valve_to_port(
        self, client: TestClient, fake_pump: FakePump
    ) -> None:
        client.post("/v1/initialize", json={})
        r = client.post("/v1/valve", json={"port": 3})
        assert r.status_code == 200
        assert r.json()["valve"] == "3"
        valve_calls = [
            c for c in fake_pump.calls if c[0] == "move_valve_to_port"
        ]
        assert valve_calls[-1] == (
            "move_valve_to_port",
            (3,),
            {"direction_ccw": False},
        )

    def test_invalid_port_returns_422(self, client: TestClient) -> None:
        # Pydantic field validation: ge=1, le=16
        r = client.post("/v1/valve", json={"port": 0})
        assert r.status_code == 422

    def test_plunger_blocked_by_bypass_returns_409(
        self, client: TestClient, fake_pump: FakePump
    ) -> None:
        fake_pump.inject_error(
            SyringePumpController.PlungerBlockedByBypassError(
                error_code=(
                    SyringePumpController.ErrorCode.PLUNGER_BLOCKED_BY_BYPASS
                ),
                command_sent="I3",
                raw_reply=b"/0K\x03",
            )
        )
        r = client.post("/v1/valve", json={"port": 3})
        assert r.status_code == 409
        assert r.json()["error"] == "PlungerBlockedByBypassError"


# -------------------------------------------------- /v1/aspirate, /v1/dispense
class TestVolume:
    def test_aspirate_uL_round_trip(
        self, client: TestClient, fake_pump: FakePump
    ) -> None:
        client.post("/v1/initialize", json={})
        r = client.post("/v1/aspirate", json={"target_uL": 62.5})
        assert r.status_code == 200
        body = r.json()
        # 62.5 µL on 125 µL syringe in NORMAL (12000) = 6000 steps
        assert body["plunger_steps"] == 6000
        assert body["target_uL"] == 62.5

    def test_dispense_uL_default_is_zero(
        self, client: TestClient, fake_pump: FakePump
    ) -> None:
        client.post("/v1/initialize", json={})
        client.post("/v1/aspirate", json={"target_uL": 125})
        r = client.post("/v1/dispense", json={})
        assert r.status_code == 200
        assert r.json()["plunger_steps"] == 0
        assert r.json()["target_uL"] == 0.0

    def test_aspirate_out_of_range_returns_400(
        self, client: TestClient
    ) -> None:
        # Driver raises ValueError before any I/O for >Config.syringe_uL.
        r = client.post("/v1/aspirate", json={"target_uL": 200})
        assert r.status_code == 400
        body = r.json()
        assert body["error"] == "ValueError"

    def test_plunger_overload_returns_500(
        self, client: TestClient, fake_pump: FakePump
    ) -> None:
        fake_pump.inject_error(
            SyringePumpController.PlungerOverloadError(
                error_code=SyringePumpController.ErrorCode.PLUNGER_OVERLOAD,
                command_sent="A12000",
                raw_reply=b"/0I\x03",
            )
        )
        r = client.post("/v1/aspirate", json={"target_uL": 125})
        assert r.status_code == 500
        body = r.json()
        assert body["error"] == "PlungerOverloadError"
        assert body["code"] == 9


# ----------------------------------------------------------- /v1/move_steps
class TestMoveSteps:
    def test_move_to_steps(
        self, client: TestClient, fake_pump: FakePump
    ) -> None:
        client.post("/v1/initialize", json={})
        r = client.post("/v1/move_steps", json={"steps": 3000})
        assert r.status_code == 200
        assert r.json()["plunger_steps"] == 3000


# ----------------------------------------------------------------- /v1/prime
class TestPrime:
    def test_single_cycle_sequence(
        self, client: TestClient, fake_pump: FakePump
    ) -> None:
        client.post("/v1/initialize", json={})
        before = len(fake_pump.calls)
        r = client.post("/v1/prime", json={"cycles": 1})
        assert r.status_code == 200
        body = r.json()
        assert body["cycles_done"] == 1
        assert body["ul_per_stroke"] == 125  # default = full syringe
        assert body["final_valve"] == "1"
        assert body["final_plunger"] == 0
        # The valve is aligned to the sink and the syringe emptied first
        # (precondition), then each cycle is
        # valve→source, aspirate, valve→sink, dispense→0.
        seq = [c[0] for c in fake_pump.calls[before:]]
        assert seq == [
            "move_valve_to_port",  # precondition: → sink
            "dispense_uL",  # precondition: empty to 0
            "move_valve_to_port",  # → source
            "aspirate_uL",
            "move_valve_to_port",  # → sink
            "dispense_uL",
        ]
        # Default aspirate is a full syringe; dispense empties to 0.
        asp = [c for c in fake_pump.calls[before:] if c[0] == "aspirate_uL"]
        assert asp[0][1] == (125,)
        disp = [c for c in fake_pump.calls[before:] if c[0] == "dispense_uL"]
        assert all(c[1] == (0,) for c in disp)

    def test_three_cycles(
        self, client: TestClient, fake_pump: FakePump
    ) -> None:
        client.post("/v1/initialize", json={})
        before = len(fake_pump.calls)
        r = client.post("/v1/prime", json={"cycles": 3})
        assert r.status_code == 200
        assert r.json()["cycles_done"] == 3
        calls = fake_pump.calls[before:]
        # One aspirate per cycle; one dispense per cycle plus the
        # precondition empty.
        assert len([c for c in calls if c[0] == "aspirate_uL"]) == 3
        assert len([c for c in calls if c[0] == "dispense_uL"]) == 4

    def test_volume_uL_sets_per_cycle_aspirate(
        self, client: TestClient, fake_pump: FakePump
    ) -> None:
        client.post("/v1/initialize", json={})
        before = len(fake_pump.calls)
        r = client.post("/v1/prime", json={"cycles": 2, "volume_uL": 60})
        assert r.status_code == 200
        assert r.json()["ul_per_stroke"] == 60
        asp = [c for c in fake_pump.calls[before:] if c[0] == "aspirate_uL"]
        assert [c[1] for c in asp] == [(60,), (60,)]

    def test_volume_uL_clamps_to_syringe(
        self, client: TestClient, fake_pump: FakePump
    ) -> None:
        client.post("/v1/initialize", json={})
        r = client.post("/v1/prime", json={"cycles": 1, "volume_uL": 999})
        assert r.status_code == 200
        # Cannot aspirate past a full stroke — clamped to the syringe size.
        assert r.json()["ul_per_stroke"] == 125

    def test_invalid_source_port_returns_422(self, client: TestClient) -> None:
        r = client.post("/v1/prime", json={"source_port": 99})
        assert r.status_code == 422


# ----------------------------------------------------------------- /v1/status
class TestStatus:
    def test_pre_init_status(self, client: TestClient) -> None:
        body = client.get("/v1/status").json()
        assert body["busy"] is False
        assert body["error_name"] == "NOT_INITIALIZED"
        assert body["error_code"] == 7
        assert body["plunger_steps"] == 0

    def test_post_init_status(self, client: TestClient) -> None:
        client.post("/v1/initialize", json={})
        body = client.get("/v1/status").json()
        assert body["error_name"] == "OK"
        assert body["error_code"] == 0
        assert body["valve"] == "1"


# ----------------------------------------------------------- error mapping
class TestErrorMapping:
    def test_transport_timeout_returns_504(
        self, client: TestClient, fake_pump: FakePump
    ) -> None:
        fake_pump.inject_error(
            SyringePumpController.TransportTimeout(
                elapsed_s=10.0,
                frame_sent=b"/1A12000\r",
                partial=b"",
            )
        )
        r = client.post("/v1/move_steps", json={"steps": 12000})
        assert r.status_code == 504
        assert r.json()["error"] == "TransportTimeout"

    def test_protocol_error_returns_502(
        self, client: TestClient, fake_pump: FakePump
    ) -> None:
        fake_pump.inject_error(
            SyringePumpController.ProtocolError("malformed status byte")
        )
        r = client.get("/v1/diagnose")
        assert r.status_code == 502
        assert r.json()["error"] == "ProtocolError"

    def test_value_error_returns_400(
        self, client: TestClient, fake_pump: FakePump
    ) -> None:
        fake_pump.inject_error(ValueError("bogus"))
        r = client.post("/v1/move_steps", json={"steps": 100})
        assert r.status_code == 400
        body = r.json()
        assert body["error"] == "ValueError"
        assert body["message"] == "bogus"

    def test_error_body_has_no_traceback(
        self, client: TestClient, fake_pump: FakePump
    ) -> None:
        fake_pump.inject_error(
            SyringePumpController.PlungerOverloadError(
                error_code=SyringePumpController.ErrorCode.PLUNGER_OVERLOAD,
                command_sent="A12000",
                raw_reply=b"/0I\x03",
            )
        )
        r = client.post("/v1/aspirate", json={"target_uL": 125})
        body = r.json()
        assert "Traceback" not in body.get("message", "")
        assert set(body.keys()) == {
            "error",
            "code",
            "command",
            "raw_reply_hex",
            "message",
        }
