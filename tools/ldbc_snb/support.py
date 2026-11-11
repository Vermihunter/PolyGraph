"""Pure LDBC compatibility helpers; generic graph commands do not import catalogs."""
from __future__ import annotations
from pathlib import Path
import re

def _normalize_scale_factor(value: str) -> str:
    """Canonicalize user-facing SF spellings to the on-disk sfX_Y form.

    Examples: 1, 1.0, sf1 and sf1_0 all become sf1_0; 0.1 becomes sf0_1.
    """
    normalized = value.strip().lower()
    if normalized.startswith("sf"):
        normalized = normalized[2:]
    normalized = normalized.replace("_", ".")
    if not re.fullmatch(r"[0-9]+(?:\.[0-9]+)?", normalized):
        raise ValueError(f"Invalid scale factor: {value!r}")
    whole, dot, fraction = normalized.partition(".")
    whole = str(int(whole))
    if not dot:
        fraction = "0"
    else:
        fraction = fraction.rstrip("0") or "0"
    return f"sf{whole}_{fraction}"


def sf_key(scale_factor: str) -> str:
    return _normalize_scale_factor(scale_factor)


def infer_scale_factor(path: Path) -> str | None:
    for part in (path.name, *[parent.name for parent in path.parents[:3]]):
        match = re.search(r"sf([0-9]+(?:\.[0-9]+)?)", part, flags=re.IGNORECASE)
        if match:
            return match.group(1)
    return None


def resolve_social_network_dir(input_path: str | Path) -> Path:
    candidate = Path(input_path).expanduser().resolve()
    if not candidate.is_dir():
        raise FileNotFoundError(f"LDBC dataset directory does not exist: {candidate}")
    if (candidate / "static").is_dir() and (candidate / "dynamic").is_dir():
        return candidate
    nested = candidate / "social_network"
    if (nested / "static").is_dir() and (nested / "dynamic").is_dir():
        return nested
    matches = sorted(
        parent
        for dynamic in candidate.glob("**/dynamic")
        if dynamic.is_dir() and (parent := dynamic.parent).joinpath("static").is_dir()
    )
    unique = list(dict.fromkeys(matches))
    if len(unique) == 1:
        return unique[0]
    if not unique:
        raise FileNotFoundError(
            f"Could not find sibling static/ and dynamic/ directories below {candidate}"
        )
    rendered = "\n".join(f"  {item}" for item in unique)
    raise ValueError(f"More than one social_network directory was found:\n{rendered}")


def update_datagen_params(path: Path, scale_factor: str, partitions: int) -> None:
    lines = path.read_text(encoding="utf-8").splitlines()
    result: list[str] = []
    scale_written = False
    partition_written = False
    for line in lines:
        if line.startswith("ldbc.snb.datagen.generator.scaleFactor:"):
            result.append(f"ldbc.snb.datagen.generator.scaleFactor:snb.interactive.{scale_factor}")
            scale_written = True
        elif line.startswith("ldbc.snb.datagen.serializer.numUpdatePartitions:"):
            result.append(f"ldbc.snb.datagen.serializer.numUpdatePartitions:{partitions}")
            partition_written = True
        else:
            result.append(line)
    if not scale_written:
        result.append(f"ldbc.snb.datagen.generator.scaleFactor:snb.interactive.{scale_factor}")
    if not partition_written:
        result.append(f"ldbc.snb.datagen.serializer.numUpdatePartitions:{partitions}")
    path.write_text("\n".join(result) + "\n", encoding="utf-8")


def default_dataset_root(data_root: Path, scale: str) -> Path:
    return data_root / "ldbc-snb" / scale


def default_domain_reference(scale: str, distribution: str) -> str:
    return f"data/ldbc-snb/{scale}/{distribution}/domain.json"


def datagen_dockerfile(repository_root: Path) -> Path:
    return repository_root / "tools/ldbc_snb/Dockerfile.datagen"
