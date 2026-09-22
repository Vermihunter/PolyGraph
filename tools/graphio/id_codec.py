from __future__ import annotations

from dataclasses import dataclass
import hashlib
import json
from pathlib import Path
import uuid as uuid_lib
from typing import Iterable, Mapping, Sequence

# Stable storage backend ids synchronized with include/utilities/BackendIds.hpp.
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
    "Redis": 5,
    "REDIS": 5,
    "MMap": 6,
    "MMAP": 6,
    "MemoryMapped": 6,
    "PostgreSQL": 7,
    "Postgres": 7,
    "POSTGRESQL": 7,
}

CANONICAL_NAMES: dict[int, str] = {
    1: "GPU",
    2: "Memgraph",
    3: "Neo4J",
    4: "ArangoDB",
    5: "Redis",
    6: "MMap",
    7: "PostgreSQL",
}
CANONICAL_ROUTING_ORDER: tuple[str, ...] = ("Neo4J", "ArangoDB", "PostgreSQL", "Memgraph", "GPU")
DEFAULT_ROUTING_BACKENDS: tuple[str, ...] = ("Neo4J", "ArangoDB", "GPU")

UINT64_MASK = (1 << 64) - 1
INT64_SIGN = 1 << 63

# GlobalId layout (v2): high 16 bits are a location/residency mask; low 48 bits are
# the stable backend-local payload.  The mask contains exactly one authoritative
# owner bit (except a GPU-primary object, which uses its GPU bit as owner) and zero
# or more cache bits.
LOCATION_BITS = 16
PAYLOAD_BITS = 64 - LOCATION_BITS
PAYLOAD_MASK = (1 << PAYLOAD_BITS) - 1
LOCATION_MASK = (1 << LOCATION_BITS) - 1

LOCATION_NEO4J = 1 << 0
LOCATION_ARANGO = 1 << 1
LOCATION_MEMGRAPH = 1 << 2
LOCATION_REDIS = 1 << 3
LOCATION_MMAP = 1 << 4
GPU_BASE_BIT = 5
POSTGRESQL_BIT = 15
LOCATION_POSTGRESQL = 1 << POSTGRESQL_BIT
MAX_GPU_DEVICES = POSTGRESQL_BIT - GPU_BASE_BIT  # device ids 0..9; bit 15 owns PostgreSQL
GPU_LOCATION_MASK = ((1 << MAX_GPU_DEVICES) - 1) << GPU_BASE_BIT
PERSISTENT_OWNER_MASK = LOCATION_NEO4J | LOCATION_ARANGO | LOCATION_MEMGRAPH | LOCATION_POSTGRESQL
CACHE_LOCATION_MASK = LOCATION_REDIS | LOCATION_MMAP | GPU_LOCATION_MASK

_OWNER_LOCATION_BY_NAME = {
    "Neo4J": LOCATION_NEO4J,
    "ArangoDB": LOCATION_ARANGO,
    "Memgraph": LOCATION_MEMGRAPH,
    "PostgreSQL": LOCATION_POSTGRESQL,
}


def gpu_location_bit(device_id: int) -> int:
    device = int(device_id)
    if not 0 <= device < MAX_GPU_DEVICES:
        raise ValueError(
            f"NVIDIA GPU id {device} cannot be encoded; supported range is 0..{MAX_GPU_DEVICES - 1}"
        )
    return 1 << (GPU_BASE_BIT + device)


def parse_global_id(value: int | str) -> int:
    """Parse an unsigned 64-bit ID from JSON/CLI input."""
    if isinstance(value, uuid_lib.UUID):
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


def location_mask(value: int | str) -> int:
    return (parse_global_id(value) >> PAYLOAD_BITS) & LOCATION_MASK


def local_payload(value: int | str) -> int:
    return parse_global_id(value) & PAYLOAD_MASK


def cache_residency(value: int | str) -> int:
    mask = location_mask(value)
    owner = owner_location_mask(value)
    # For GPU-primary IDs, the least-significant GPU bit is ownership; other
    # GPU/cache bits still describe additional residency.
    return (mask & CACHE_LOCATION_MASK) & ~owner


def owner_location_mask(value: int | str) -> int:
    mask = location_mask(value)
    persistent = mask & PERSISTENT_OWNER_MASK
    if persistent:
        if persistent & (persistent - 1):
            raise ValueError(f"Global ID has more than one persistent owner bit: 0x{mask:x}")
        return persistent
    gpu = mask & GPU_LOCATION_MASK
    if gpu:
        return gpu & -gpu
    raise ValueError(f"Global ID has no authoritative owner bit: 0x{mask:x}")


def canonical_global_id(value: int | str) -> int:
    parsed = parse_global_id(value)
    return (owner_location_mask(parsed) << PAYLOAD_BITS) | local_payload(parsed)


def with_cache_residency(value: int | str, residency_mask: int) -> int:
    parsed = parse_global_id(value)
    owner = owner_location_mask(parsed)
    cache = int(residency_mask) & CACHE_LOCATION_MASK
    # A GPU-primary object already carries the GPU ownership bit; OR is harmless.
    return ((owner | cache) << PAYLOAD_BITS) | local_payload(parsed)


def has_location(value: int | str, mask: int) -> bool:
    return bool(location_mask(value) & int(mask))


@dataclass(frozen=True)
class BackendLayout:
    """Authoritative source-selection layout plus fixed MSB residency encoding."""

    backends: tuple[str, ...]

    def __post_init__(self) -> None:
        canonical = tuple(canonical_backend_name(value) for value in self.backends)
        allowed = {"Neo4J", "ArangoDB", "PostgreSQL", "Memgraph", "GPU"}
        if not canonical:
            raise ValueError("ID routing layout must contain at least one authoritative backend")
        if len(set(canonical)) != len(canonical):
            raise ValueError(f"ID routing layout contains duplicates: {canonical}")
        if set(canonical) - allowed:
            raise ValueError(f"Cache-only locations cannot own IDs: {sorted(set(canonical) - allowed)}")
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
        return LOCATION_BITS

    @property
    def payload_bits(self) -> int:
        return PAYLOAD_BITS

    @property
    def payload_mask(self) -> int:
        return PAYLOAD_MASK

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

    def _owner_mask_for_source(self, source_or_id: str | int) -> int:
        name = canonical_backend_name(source_or_id)
        if name == "GPU":
            return gpu_location_bit(0)
        try:
            return _OWNER_LOCATION_BY_NAME[name]
        except KeyError as exc:
            raise ValueError(f"Unsupported authoritative backend: {name}") from exc

    def encode(self, value: int | str, source_or_id: str | int, *, residency_mask: int = 0) -> int:
        local_id = parse_global_id(value) & PAYLOAD_MASK
        self.code_for(source_or_id)  # validate configured ownership source
        owner = self._owner_mask_for_source(source_or_id)
        cache = int(residency_mask) & CACHE_LOCATION_MASK
        return ((owner | cache) << PAYLOAD_BITS) | local_id

    def encode_local(self, local_id: int | str, source_or_id: str | int, *, residency_mask: int = 0) -> int:
        parsed = parse_global_id(local_id)
        if parsed > PAYLOAD_MASK:
            raise OverflowError(f"Backend-local ID {parsed} requires more than {PAYLOAD_BITS} payload bits")
        return self.encode(parsed, source_or_id, residency_mask=residency_mask)

    def local_id(self, value: int | str) -> int:
        return local_payload(value)

    def decode_code(self, value: int | str) -> int:
        return self.code_for(self.decode_source(value))

    def decode_source(self, value: int | str) -> str:
        owner = owner_location_mask(value)
        if owner == LOCATION_NEO4J:
            name = "Neo4J"
        elif owner == LOCATION_ARANGO:
            name = "ArangoDB"
        elif owner == LOCATION_MEMGRAPH:
            name = "Memgraph"
        elif owner == LOCATION_POSTGRESQL:
            name = "PostgreSQL"
        elif owner & GPU_LOCATION_MASK:
            name = "GPU"
        else:
            raise ValueError(f"Unknown owner location bit 0x{owner:x}")
        if name not in self.backends:
            raise ValueError(f"Global ID owner {name} is not in routing layout {self.backends}")
        return name

    def decode_backend_id(self, value: int | str) -> int:
        return backend_id(self.decode_source(value))

    def cache_residency(self, value: int | str) -> int:
        return cache_residency(value)

    def canonical_id(self, value: int | str) -> int:
        return canonical_global_id(value)

    def decorate(self, value: int | str, residency_mask: int) -> int:
        return with_cache_residency(value, residency_mask)

    def metadata(self) -> dict[str, object]:
        return {
            "idRouting": list(self.backends),
            "idRoutingBits": LOCATION_BITS,
            "idRoutingPosition": "msb",
            "idLocationEncoding": "bitmask-msb-v1",
            "idLocationBits": LOCATION_BITS,
            "idPayloadBits": PAYLOAD_BITS,
            "idType": "uint64",
            "idLocations": {
                "Neo4J": 0,
                "ArangoDB": 1,
                "Memgraph": 2,
                "Redis": 3,
                "MMap": 4,
                "GPUBase": GPU_BASE_BIT,
                "maxGpuDevices": MAX_GPU_DEVICES,
            },
        }


def _layout(routing_backends: Sequence[str] | BackendLayout | None) -> BackendLayout:
    if isinstance(routing_backends, BackendLayout):
        return routing_backends
    return BackendLayout(tuple(routing_backends or DEFAULT_ROUTING_BACKENDS))


def encode_backend_id(value: int | str, source_or_id: str | int,
                      routing_backends: Sequence[str] | BackendLayout | None = None) -> int:
    return _layout(routing_backends).encode(value, source_or_id)


def decode_backend_id(value: int | str,
                      routing_backends: Sequence[str] | BackendLayout | None = None) -> int:
    return _layout(routing_backends).decode_backend_id(value)


def decode_backend_source(value: int | str,
                          routing_backends: Sequence[str] | BackendLayout | None = None) -> str:
    return _layout(routing_backends).decode_source(value)


def deterministic_routed_entity_id(entity_kind: str, entity_type: str, external_id: str | int,
                                   routing_backends: Sequence[str] | BackendLayout | None = None) -> tuple[int, str]:
    layout = _layout(routing_backends)
    identity = f"{entity_kind}:{entity_type}:{external_id}"
    source = layout.source_for_identity(identity)
    return layout.encode(BackendLayout.stable_hash(identity), source), source


def deterministic_entity_id(source: str, entity_kind: str, entity_type: str, external_id: str | int,
                            routing_backends: Sequence[str] | BackendLayout | None = None) -> int:
    identity = f"{entity_kind}:{entity_type}:{external_id}"
    return _layout(routing_backends).encode(BackendLayout.stable_hash(identity), source)


deterministic_routed_entity_uuid = deterministic_routed_entity_id
deterministic_entity_uuid = deterministic_entity_id


def write_native_id_map(path: str | Path, *, node_mapping: Mapping[int, int],
                        relationship_mapping: Mapping[int, int] | None = None) -> Path:
    output = Path(path).expanduser().resolve()
    output.parent.mkdir(parents=True, exist_ok=True)
    temporary = output.with_name(f".{output.name}.tmp")
    with temporary.open("w", encoding="utf-8") as handle:
        handle.write(json.dumps({
            "record": "metadata", "format": "polygraph.native-id-map", "version": 1, "idType": "uint64"
        }, separators=(",", ":")) + "\n")
        for old_id, new_id in sorted(node_mapping.items()):
            handle.write(json.dumps({"record": "node", "old": parse_global_id(old_id), "new": parse_global_id(new_id)},
                                    separators=(",", ":")) + "\n")
        for old_id, new_id in sorted((relationship_mapping or {}).items()):
            handle.write(json.dumps({"record": "relationship", "old": parse_global_id(old_id), "new": parse_global_id(new_id)},
                                    separators=(",", ":")) + "\n")
    temporary.replace(output)
    return output


def read_native_node_id_map(path: str | Path | None) -> dict[int, int]:
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
                if record.get("format") != "polygraph.native-id-map" or record.get("version") != 1:
                    raise ValueError(f"Unsupported native ID map: {source}")
                continue
            if record.get("record") == "node":
                result[parse_global_id(record["old"])] = parse_global_id(record["new"])
    return result


def resolve_native_node_id(value: int | str, mapping: Mapping[int, int]) -> int:
    parsed = parse_global_id(value)
    return mapping.get(parsed, parsed)
