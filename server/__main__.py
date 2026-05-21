"""``python -m server`` entry point.

Reads a TOML config (``[pump]`` for ``SyringePumpController.Config``,
``[server]`` for host/port/log level), opens the pump once, and hands
the live app to uvicorn. Single worker — multiple workers would each
try to open ``/dev/ttyUSB1`` and fight for the serial handle.
"""

from __future__ import annotations

import argparse
import os
import tomllib
from dataclasses import dataclass
from pathlib import Path

import uvicorn

from server.app import create_app
from sy01b import SyringePumpController


@dataclass(frozen=True, slots=True)
class ServerConfig:
    host: str = "0.0.0.0"
    port: int = 17046
    log_level: str = "info"


def _load(path: Path) -> tuple[SyringePumpController.Config, ServerConfig]:
    pump_cfg = SyringePumpController.Config.from_toml(path)
    raw = tomllib.loads(path.read_text(encoding="utf-8"))
    server_section = raw.get("server", {})
    server_cfg = ServerConfig(
        host=server_section.get("host", "0.0.0.0"),
        port=int(server_section.get("port", 17046)),
        log_level=server_section.get("log_level", "info"),
    )
    return pump_cfg, server_cfg


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(prog="sy01b-server")
    parser.add_argument(
        "--config",
        type=Path,
        default=None,
        help=(
            "TOML config path. Default: $SY01B_SERVER_CONFIG env var, "
            "otherwise ./server/pump.toml."
        ),
    )
    args = parser.parse_args(argv)

    if args.config is not None:
        cfg_path = args.config
    elif env := os.environ.get("SY01B_SERVER_CONFIG"):
        cfg_path = Path(env)
    else:
        cfg_path = Path("server/pump.toml")

    if not cfg_path.exists():
        parser.error(f"config file not found: {cfg_path}")
    pump_cfg, server_cfg = _load(cfg_path)

    app = create_app(
        pump_factory=lambda: SyringePumpController.open(pump_cfg),
        config=pump_cfg,
    )
    uvicorn.run(
        app,
        host=server_cfg.host,
        port=server_cfg.port,
        log_level=server_cfg.log_level,
        timeout_keep_alive=120,
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
