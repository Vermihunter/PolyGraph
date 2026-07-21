from __future__ import annotations

from dataclasses import dataclass
import hashlib
import json
from pathlib import Path
import uuid as uuid_lib
from typing import Iterable, Mapping, Sequence

# Keep stable IDs synchronized with include/utilities/BackendIds.hpp.
BACKEND_IDS: dict[str, int] = {
    "GPU": 1,
    "CuGraph": 1,
    "cuGraph": 1,
    "Memgraph": 2,
    "Neo4J": 3,
    "Neo4j": 3,
    "NEO4J": 3,
    "ArangoDB": 4,
    "Arango": 4,
    "ARANGO": 4,
}

CANONICAL_NAMES: dict[int, str] = {
    1: "GPU",
    2: "Memgraph",
    3: "Neo4J",
    4: "ArangoDB",
}
CANONICAL_ROUTING_ORDER: tuple[str, ...] = ("Neo4J", "ArangoDB", "Memgraph", "GPU")
DEFAULT_ROUTING_BACKENDS: tuple[str, ...] = ("Neo4J", "ArangoDB", "GPU")
UINT64_MASK = (1 << 64) - 1
INT64_SIGN = 1 << 63


def parse_global_id(value: int | str) -> int:
    """Parse an unsigned 64-bit ID from JSON/CLI input.

    Negative values are accepted as signed database representations and restored
    to their original unsigned two's-complement bit pattern.
    """
    if isinstance(value, uuid_lib.UUID):
        # Legacy-input compatibility only. Canonical graph output is always a
        # numeric uint64; reducing here lets the migration pipeline consume old
        # UUID JSON without carrying UUID objects into the runtime.
        return value.int & UINT64_MASK

    if isinstance(value, str):
        text = value.strip()
        try:
            parsed = int(text, 0)
        except ValueError:
            try:
                return uuid_lib.UUID(text).int & UINT64_MASK
            except (ValueError, AttributeError) as exc:
                raise ValueError(f"Invalid uint64 global ID: {value}") from exc
    else:
        parsed = int(value)

    if parsed < 0:
        parsed &= UINT64_MASK
    if not 0 <= parsed <= UINT64_MASK:
        raise ValueError(f"Global ID is outside uint64 range: {value}")
    return parsed


def to_signed_storage(value: int | str) -> int:
    """Represent an unsigned ID as a Neo4j-compatible signed INTEGER."""
    parsed = parse_global_id(value)
    return parsed - (1 << 64) if parsed & INT64_SIGN else parsed


def backend_id(source: str) -> int:
    try:
        return BACKEND_IDS[source]
    except KeyError as exc:
        raise ValueError(f"Unsupported backend source '{source}'. Known sources: {sorted(BACKEND_IDS)}") from exc


def canonical_backend_name(source_or_id: str | int) -> str:
    value_id = backend_id(source_or_id) if isinstance(source_or_id, str) else int(source_or_id)
    try:
        return CANONICAL_NAMES[value_id]
    except KeyError as exc:
        raise ValueError(f"Unsupported backend id: {value_id}") from exc


def routing_names(metadata: dict) -> tuple[str, ...] | None:
    configured = metadata.get("idRouting")
    if not configured:
        return None
    return tuple(str(value) for value in configured)


@dataclass(frozen=True)
class BackendLayout:
    """Ordered backend codes stored in the most-significant uint64 bits."""

    backends: tuple[str, ...]

    def __post_init__(self) -> None:
        canonical = tuple(canonical_backend_name(value) for value in self.backends)
        if not canonical:
            raise ValueError("ID routing layout must contain at least one backend")
        if len(set(canonical)) != len(canonical):
            raise ValueError(f"ID routing layout contains duplicates: {canonical}")
        object.__setattr__(self, "backends", canonical)

    @classmethod
    def from_sources(cls, sources: Iterable[str]) -> "BackendLayout":
        used = {canonical_backend_name(source) for source in sources}
        ordered = tuple(name for name in CANONICAL_ROUTING_ORDER if name in used)
        if len(ordered) != len(used):
            missing = sorted(used - set(ordered))
            raise ValueError(f"Backends are missing from canonical routing order: {missing}")
        return cls(ordered)

    @property
    def bit_count(self) -> int:
        return 0 if len(self.backends) == 1 else (len(self.backends) - 1).bit_length()

    @property
    def payload_bits(self) -> int:
        return 64 - self.bit_count

    @property
    def payload_mask(self) -> int:
        return UINT64_MASK if self.payload_bits == 64 else (1 << self.payload_bits) - 1

    def code_for(self, source_or_id: str | int) -> int:
        name = canonical_backend_name(source_or_id)
        try:
            return self.backends.index(name)
        except ValueError as exc:
            raise ValueError(f"Backend {name} is not in ID routing layout {self.backends}") from exc

    @staticmethod
    def stable_hash(identity: str) -> int:
        return int.from_bytes(hashlib.blake2b(identity.encode("utf-8"), digest_size=8).digest(), "big")

    def source_for_identity(self, identity: str) -> str:
        return self.source_for_code(self.stable_hash(identity) % len(self.backends))

    def source_for_code(self, code: int) -> str:
        if not 0 <= code < len(self.backends):
            raise ValueError(f"Global ID contains unused backend routing code {code}")
        return self.backends[code]

    def encode(self, value: int | str, source_or_id: str | int) -> int:
        local_id = parse_global_id(value) & self.payload_mask
        code = self.code_for(source_or_id)
        return local_id if self.payload_bits == 64 else (code << self.payload_bits) | local_id

    def encode_local(self, local_id: int | str, source_or_id: str | int) -> int:
        parsed = parse_global_id(local_id)
        if parsed > self.payload_mask:
            raise OverflowError(
                f"Backend-local ID {parsed} requires more than {self.payload_bits} payload bits"
            )
        return self.encode(parsed, source_or_id)

    def local_id(self, value: int | str) -> int:
        return parse_global_id(value) & self.payload_mask

    def decode_code(self, value: int | str) -> int:
        parsed = parse_global_id(value)
        code = 0 if self.payload_bits == 64 else parsed >> self.payload_bits
        if code >= len(self.backends):
            raise ValueError(f"Global ID contains unused backend routing code {code}")
        return code

    def decode_source(self, value: int | str) -> str:
        return self.source_for_code(self.decode_code(value))

    def decode_backend_id(self, value: int | str) -> int:
        return backend_id(self.decode_source(value))

    def metadata(self) -> dict[str, object]:
        return {
            "idRouting": list(self.backends),
            "idRoutingBits": self.bit_count,
            "idRoutingPosition": "msb",
            "idType": "uint64",
        }


def _layout(routing_backends: Sequence[str] | BackendLayout | None) -> BackendLayout:
    if isinstance(routing_backends, BackendLayout):
        return routing_backends
    return BackendLayout(tuple(routing_backends or DEFAULT_ROUTING_BACKENDS))


def encode_backend_id(
    value: int | str,
    source_or_id: str | int,
    routing_backends: Sequence[str] | BackendLayout | None = None,
) -> int:
    return _layout(routing_backends).encode(value, source_or_id)


def decode_backend_id(
    value: int | str,
    routing_backends: Sequence[str] | BackendLayout | None = None,
) -> int:
    return _layout(routing_backends).decode_backend_id(value)


def decode_backend_source(
    value: int | str,
    routing_backends: Sequence[str] | BackendLayout | None = None,
) -> str:
    return _layout(routing_backends).decode_source(value)


def deterministic_routed_entity_id(
    entity_kind: str,
    entity_type: str,
    external_id: str | int,
    routing_backends: Sequence[str] | BackendLayout | None = None,
) -> tuple[int, str]:
    layout = _layout(routing_backends)
    identity = f"{entity_kind}:{entity_type}:{external_id}"
    source = layout.source_for_identity(identity)
    return layout.encode(BackendLayout.stable_hash(identity), source), source


def deterministic_entity_id(
    source: str,
    entity_kind: str,
    entity_type: str,
    external_id: str | int,
    routing_backends: Sequence[str] | BackendLayout | None = None,
) -> int:
    identity = f"{entity_kind}:{entity_type}:{external_id}"
    return _layout(routing_backends).encode(BackendLayout.stable_hash(identity), source)


# Compatibility names retained for existing imports; values are numeric uint64 IDs.
deterministic_routed_entity_uuid = deterministic_routed_entity_id
deterministic_entity_uuid = deterministic_entity_id


def write_native_id_map(
    path: str | Path,
    *,
    node_mapping: Mapping[int, int],
    relationship_mapping: Mapping[int, int] | None = None,
) -> Path:
    """Write a streaming old-global-ID -> resolved-native-global-ID sidecar."""
    output = Path(path).expanduser().resolve()
    output.parent.mkdir(parents=True, exist_ok=True)
    temporary = output.with_name(f".{output.name}.tmp")
    with temporary.open("w", encoding="utf-8") as handle:
        handle.write(json.dumps({
            "record": "metadata",
            "format": "cpgdb.native-id-map",
            "version": 1,
            "idType": "uint64",
        }, separators=(",", ":")) + "\n")
        for old_id, new_id in sorted(node_mapping.items()):
            handle.write(json.dumps({
                "record": "node",
                "old": parse_global_id(old_id),
                "new": parse_global_id(new_id),
            }, separators=(",", ":")) + "\n")
        for old_id, new_id in sorted((relationship_mapping or {}).items()):
            handle.write(json.dumps({
                "record": "relationship",
                "old": parse_global_id(old_id),
                "new": parse_global_id(new_id),
            }, separators=(",", ":")) + "\n")
    temporary.replace(output)
    return output


def read_native_node_id_map(path: str | Path | None) -> dict[int, int]:
    """Read only node mappings from a native-ID sidecar."""
    if path is None:
        return {}
    source = Path(path).expanduser().resolve()
    if not source.is_file():
        return {}
    result: dict[int, int] = {}
    with source.open("r", encoding="utf-8") as handle:
        for line_number, line in enumerate(handle, start=1):
            line = line.strip()
            if not line:
                continue
            record = json.loads(line)
            if line_number == 1 and record.get("record") == "metadata":
                if record.get("format") != "cpgdb.native-id-map" or record.get("version") != 1:
                    raise ValueError(f"Unsupported native ID map: {source}")
                continue
            if record.get("record") == "node":
                result[parse_global_id(record["old"])] = parse_global_id(record["new"])
    return result


def resolve_native_node_id(value: int | str, mapping: Mapping[int, int]) -> int:
    parsed = parse_global_id(value)
    return mapping.get(parsed, parsed)
