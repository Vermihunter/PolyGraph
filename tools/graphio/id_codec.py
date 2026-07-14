from __future__ import annotations

from dataclasses import dataclass
from typing import Iterable, Sequence
import uuid

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

# Stable, project-owned UUID namespace. Changing this breaks reproducibility.
CPGDB_ENTITY_NAMESPACE = uuid.UUID("71ee8ed4-5f45-5dbf-b89d-56ce573bf19d")


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


@dataclass(frozen=True)
class BackendLayout:
    """Ordered backend codes stored in the least-significant UUID bits."""

    backends: tuple[str, ...]

    def __post_init__(self) -> None:
        canonical = tuple(canonical_backend_name(value) for value in self.backends)
        if not canonical:
            raise ValueError("UUID routing layout must contain at least one backend")
        if len(set(canonical)) != len(canonical):
            raise ValueError(f"UUID routing layout contains duplicates: {canonical}")
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

    def code_for(self, source_or_id: str | int) -> int:
        name = canonical_backend_name(source_or_id)
        try:
            return self.backends.index(name)
        except ValueError as exc:
            raise ValueError(f"Backend {name} is not in UUID routing layout {self.backends}") from exc

    def source_for_identity(self, identity: str) -> str:
        """Choose a backend deterministically without relying on logical node/edge types.

        The UUIDv5 payload is uniformly distributed. Bits reserved for backend routing are
        skipped before taking modulo ``B``, so the selected code is always in [0, B) even
        when ``B`` is not a power of two (for example B=3 uses two UUID suffix bits).
        """
        stable = uuid.uuid5(CPGDB_ENTITY_NAMESPACE, identity)
        code = (stable.int >> self.bit_count) % len(self.backends)
        return self.source_for_code(code)

    def source_for_code(self, code: int) -> str:
        if not 0 <= code < len(self.backends):
            raise ValueError(f"UUID contains unused backend routing code {code}")
        return self.backends[code]

    def encode(self, value: uuid.UUID, source_or_id: str | int) -> uuid.UUID:
        raw = bytearray(value.bytes)
        code = self.code_for(source_or_id)
        for bit in range(self.bit_count):
            byte_index = 15 - bit // 8
            mask = 1 << (bit % 8)
            if code & (1 << bit):
                raw[byte_index] |= mask
            else:
                raw[byte_index] &= ~mask
        return uuid.UUID(bytes=bytes(raw))

    def decode_code(self, value: str | uuid.UUID) -> int:
        parsed = value if isinstance(value, uuid.UUID) else uuid.UUID(str(value))
        code = 0
        for bit in range(self.bit_count):
            byte_index = 15 - bit // 8
            if parsed.bytes[byte_index] & (1 << (bit % 8)):
                code |= 1 << bit
        if code >= len(self.backends):
            raise ValueError(f"UUID contains unused backend routing code {code}")
        return code

    def decode_source(self, value: str | uuid.UUID) -> str:
        return self.source_for_code(self.decode_code(value))

    def decode_backend_id(self, value: str | uuid.UUID) -> int:
        return backend_id(self.decode_source(value))

    def metadata(self) -> dict[str, object]:
        return {"uuidRouting": list(self.backends), "uuidRoutingBits": self.bit_count}


def _layout(routing_backends: Sequence[str] | BackendLayout | None) -> BackendLayout:
    if isinstance(routing_backends, BackendLayout):
        return routing_backends
    return BackendLayout(tuple(routing_backends or DEFAULT_ROUTING_BACKENDS))


def encode_backend_id(
    value: uuid.UUID,
    source_or_id: str | int,
    routing_backends: Sequence[str] | BackendLayout | None = None,
) -> uuid.UUID:
    """Encode the backend's position in the configured layout into UUID suffix bits."""
    return _layout(routing_backends).encode(value, source_or_id)


def decode_backend_id(
    value: str | uuid.UUID,
    routing_backends: Sequence[str] | BackendLayout | None = None,
) -> int:
    return _layout(routing_backends).decode_backend_id(value)


def decode_backend_source(
    value: str | uuid.UUID,
    routing_backends: Sequence[str] | BackendLayout | None = None,
) -> str:
    return _layout(routing_backends).decode_source(value)


def deterministic_routed_entity_uuid(
    entity_kind: str,
    entity_type: str,
    external_id: str | int,
    routing_backends: Sequence[str] | BackendLayout | None = None,
) -> tuple[str, str]:
    """Create a reproducible UUID and choose its storage backend from the identity.

    This is the default generator path for type-independent storage placement. The
    returned source is descriptive only; runtime routing decodes the UUID suffix.
    """
    layout = _layout(routing_backends)
    identity = f"{entity_kind}:{entity_type}:{external_id}"
    source = layout.source_for_identity(identity)
    stable = uuid.uuid5(CPGDB_ENTITY_NAMESPACE, identity)
    return str(layout.encode(stable, source)), source


def deterministic_entity_uuid(
    source: str,
    entity_kind: str,
    entity_type: str,
    external_id: str | int,
    routing_backends: Sequence[str] | BackendLayout | None = None,
) -> str:
    """Create a reproducible UUID whose suffix routes to the entity's home backend."""
    identity = f"{entity_kind}:{entity_type}:{external_id}"
    stable = uuid.uuid5(CPGDB_ENTITY_NAMESPACE, identity)
    return str(encode_backend_id(stable, source, routing_backends))
