"""Make the ``capture`` package importable from the tests."""

import sys
from pathlib import Path

_PKG_DIR = Path(__file__).resolve().parent.parent / "ble-data-capture"
if str(_PKG_DIR) not in sys.path:
    sys.path.insert(0, str(_PKG_DIR))
