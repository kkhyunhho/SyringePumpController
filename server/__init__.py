"""FastAPI HTTP bridge between an ESP32 client and SyringePumpController.

Layered as a thin adapter: every endpoint delegates to one method on a single
``SyringePumpController`` instance held in ``app.state.pump``. Driver-level
safety patterns (diagnose-before-init, position polling) are unchanged.
"""

from server.app import create_app

__all__ = ["create_app"]
