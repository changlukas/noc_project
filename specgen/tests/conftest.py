"""pytest config — add specgen/ to sys.path so tests can `from ni_spec import ...`."""
import sys
from pathlib import Path

SPECGEN_ROOT = Path(__file__).resolve().parent.parent
if str(SPECGEN_ROOT) not in sys.path:
    sys.path.insert(0, str(SPECGEN_ROOT))
