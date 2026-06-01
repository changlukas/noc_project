"""pytest config — add spec_validate/ to sys.path so tests can `from ni_spec import ...`."""
import sys
from pathlib import Path

SPEC_VALIDATE_ROOT = Path(__file__).resolve().parent.parent
if str(SPEC_VALIDATE_ROOT) not in sys.path:
    sys.path.insert(0, str(SPEC_VALIDATE_ROOT))
