"""One reusable offline contract executed for every declared adapter fixture."""

import json
from pathlib import Path
import unittest

from tests.support.adapter_contract import AdapterContractMixin

CASES = Path(__file__).parent / "fixtures/adapter_contract/cases.json"
for case in json.loads(CASES.read_text()):
    globals()[case["name"] + "ContractTest"] = type(
        case["name"] + "ContractTest",
        (AdapterContractMixin, unittest.TestCase),
        {"case": case, "__module__": __name__},
    )
