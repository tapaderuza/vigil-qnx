"""pytest configuration: build the host tools once and expose them to the tests."""
import subprocess
import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "sim"))


@pytest.fixture(scope="session", autouse=True)
def host_build():
    subprocess.run(["make", "-s", "host"], cwd=ROOT, check=True)
