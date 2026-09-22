#!/usr/bin/env python3

import argparse
import json
import random
import sys
from pathlib import Path
from datetime import UTC, date, datetime, timedelta
from typing import Dict, List, Tuple, Any

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))

from tools.graphio.id_codec import (
    DEFAULT_ROUTING_BACKENDS,
    BackendLayout,
    decode_backend_source,
    encode_backend_id,
)


"""
python3 sample-generator.py ../../storage_layer/sample-university-domain.graph-shape.json ../../storage_layer/sample-university-domain.config.json ../../storage_layer/sample-university-domain.sample-values.json -p large -s 123 -o large-university.json
"""


def load_json(path: str) -> Dict[str, Any]:
    with open(path, "r", encoding="utf-8") as f:
        return json.load(f)


def pool(sample_data: Dict[str, Any], key: str, fallback: List[Any]) -> List[Any]:
    value = sample_data.get(key, fallback)
    return value if isinstance(value, list) and value else fallback


def one_of(rng: random.Random, items: List[Any], fallback: Any) -> Any:
    return rng.choice(items) if items else fallback


def random_date(rng: random.Random, start_year=2020, end_year=2026) -> date:
    start = date(start_year, 1, 1)
    end = date(end_year, 12, 31)
    delta = (end - start).days
    return start + timedelta(days=rng.randint(0, delta))


def random_datetime(rng: random.Random, start_year=2020, end_year=2026) -> datetime:
    d = random_date(rng, start_year, end_year)
    return datetime(
        d.year,
        d.month,
        d.day,
        rng.randint(8, 18),
        rng.choice([0, 15, 30, 45]),
        0,
    )


def iso_date(v: date) -> str:
    return v.isoformat()


def iso_datetime(v: datetime) -> str:
    return v.isoformat()


def make_email(first: str, last: str, domain: str) -> str:
    first = first.lower().replace(" ", "")
    last = last.lower().replace(" ", "")
    return f"{first}.{last}@{domain}"


def make_routed_id(
    rng: random.Random, entity_kind: str, routing_layout: BackendLayout
) -> tuple[int, str]:
    """Generate a seeded uint64 payload and route it across configured backends."""
    identity = f"{entity_kind}:{rng.getrandbits(64)}"
    source_name = routing_layout.source_for_identity(identity)
    return routing_layout.encode(rng.getrandbits(64), source_name), source_name


def make_encoded_id(
    rng: random.Random, source_name: str, entity_kind: str, routing_layout: BackendLayout
) -> int:
    """Generate a seeded uint64 ID routed to the schema-assigned backend."""
    del entity_kind  # Entity kind is schema metadata, not an ID bit.
    return routing_layout.encode(rng.getrandbits(64), source_name)


def decode_source_from_id(global_id: int, routing_layout: BackendLayout) -> str:
    return decode_backend_source(global_id, routing_layout)


def validate_encoded_id(
    global_id: int,
    routing_layout: BackendLayout,
    expected_source: str | None = None,
) -> bool:
    try:
        actual_source = decode_source_from_id(global_id, routing_layout)
    except (TypeError, ValueError):
        return False
    return expected_source is None or actual_source == expected_source


def generate_value_for_property(
    rng: random.Random,
    prop: Dict[str, Any],
    sample_data: Dict[str, Any],
    node_type: str = "",
    relation_type: str = "",
    idx: int = 0,
) -> Any:
    name = prop.get("name", "")
    typ = prop.get("type", "string")
    sample = prop.get("sampleValue")

    first_names = pool(sample_data, "firstNames", ["Adam"])
    last_names = pool(sample_data, "lastNames", ["Novak"])
    cities = pool(sample_data, "cities", ["Prague"])
    countries = pool(sample_data, "countries", ["Czech Republic"])
    faculty_names = pool(sample_data, "facultyNames", ["Faculty"])
    department_names = pool(sample_data, "departmentNames", ["Department"])
    program_names = pool(sample_data, "programNames", ["Program"])
    degree_titles = pool(sample_data, "degreeTitles", [["Bachelor of Science", "BSc"]])
    specializations = pool(sample_data, "specializations", ["Specialization"])
    topics = pool(sample_data, "topics", ["Topic"])
    course_names = pool(sample_data, "courseNames", ["Course"])
    companies = pool(sample_data, "companies", ["Company"])
    skills = pool(sample_data, "skills", ["Skill"])
    clubs = pool(sample_data, "clubs", ["Club"])
    event_types = pool(sample_data, "eventTypes", ["seminar"])
    building_prefixes = pool(sample_data, "buildingPrefixes", ["A"])
    sponsor_types = pool(sample_data, "sponsorTypes", ["company"])
    scholarship_names = pool(sample_data, "scholarshipNames", ["Scholarship"])
    research_titles = pool(sample_data, "researchProjectTitles", ["Research Project"])
    publication_titles = pool(sample_data, "publicationTitles", ["Publication"])
    street_names = pool(sample_data, "streetNames", ["Main St"])
    languages = pool(sample_data, "languages", ["English"])
    statuses = pool(sample_data, "statuses", ["active"])
    roles = pool(sample_data, "roles", ["member"])
    categories = pool(sample_data, "categories", ["academic"])
    currencies = pool(sample_data, "currencies", ["EUR"])
    days = pool(sample_data, "daysOfWeek", ["Monday"])
    start_times = pool(sample_data, "startTimes", ["09:00"])
    end_times = pool(sample_data, "endTimes", ["10:30"])
    publication_types = pool(sample_data, "publicationTypes", ["conference-paper"])
    application_types = pool(sample_data, "applicationTypes", ["scholarship"])
    exam_types = pool(sample_data, "examTypes", ["final"])
    levels = pool(sample_data, "levels", ["intermediate"])
    domains = sample_data.get("domains", {"university": "university.edu"})

    if typ == "boolean":
        return rng.choice([True, False])

    if typ == "integer":
        lname = name.lower()
        if "year" in lname:
            return rng.randint(1990, 2026)
        if "credits" in lname:
            return rng.choice([2, 3, 4, 5, 6, 8, 10, 120])
        if "capacity" in lname:
            return rng.randint(20, 250)
        if "semester" in lname:
            return rng.randint(1, 10)
        if "attempt" in lname:
            return rng.randint(1, 3)
        if "hours" in lname:
            return rng.randint(1, 20)
        if "priority" in lname:
            return rng.randint(1, 5)
        if "duration" in lname:
            return rng.choice([45, 60, 90, 120, 180])
        if "weekscount" in lname:
            return rng.randint(8, 14)
        if "authororder" in lname:
            return rng.randint(1, 8)
        if "bednumber" in lname:
            return rng.randint(1, 4)
        return rng.randint(1, 1000)

    if typ == "float":
        lname = name.lower()
        if "gpa" in lname:
            return round(rng.uniform(1.0, 4.0), 2)
        if "amount" in lname or "budget" in lname or "cost" in lname:
            return round(rng.uniform(100.0, 5000000.0), 2)
        if "weight" in lname:
            return round(rng.uniform(5.0, 100.0), 2)
        if "points" in lname:
            return round(rng.uniform(0.0, 100.0), 1)
        if "allocationpercent" in lname:
            return round(rng.uniform(10.0, 100.0), 2)
        if "practicalweight" in lname:
            return round(rng.uniform(0.1, 1.0), 2)
        return round(rng.uniform(0.0, 1000.0), 2)

    if typ == "date":
        return iso_date(random_date(rng))

    if typ == "datetime":
        return iso_datetime(random_datetime(rng))

    lname = name.lower()

    if "email" in lname:
        first = one_of(rng, first_names, "Adam")
        last = one_of(rng, last_names, "Novak")
        domain = domains.get("university", "university.edu")
        return make_email(first, last, domain)

    if "website" in lname:
        base = node_type.lower().replace(" ", "-")
        return f"https://{base}-{idx}.example.com"

    if "country" in lname:
        return one_of(rng, countries, "Czech Republic")

    if "city" in lname:
        return one_of(rng, cities, "Prague")

    if "firstname" in lname:
        return one_of(rng, first_names, "Adam")

    if "lastname" in lname:
        return one_of(rng, last_names, "Novak")

    if "fullname" in lname:
        return f"{one_of(rng, first_names, 'Adam')} {one_of(rng, last_names, 'Novak')}"

    if lname == "name":
        if node_type == "Faculty":
            return one_of(rng, faculty_names, f"Faculty {idx}")
        if node_type == "Department":
            return one_of(rng, department_names, f"Department {idx}")
        if node_type == "Program":
            return one_of(rng, program_names, f"Program {idx}")
        if node_type == "Specialization":
            return one_of(rng, specializations, f"Specialization {idx}")
        if node_type == "Topic":
            return one_of(rng, topics, f"Topic {idx}")
        if node_type == "Course":
            return one_of(rng, course_names, f"Course {idx}")
        if node_type == "Club":
            return one_of(rng, clubs, f"Club {idx}")
        if node_type == "Company":
            return one_of(rng, companies, f"Company {idx}")
        if node_type == "Skill":
            return one_of(rng, skills, f"Skill {idx}")
        if node_type == "Scholarship":
            return one_of(rng, scholarship_names, f"Scholarship {idx}")
        if node_type == "Campus":
            return f"{one_of(rng, cities, 'Prague')} Campus"
        if node_type == "Building":
            return f"Building {one_of(rng, building_prefixes, 'A')}{rng.randint(1, 30)}"
        if node_type == "University":
            return "Charles University"
        return f"{node_type} {idx}"

    if "title" in lname:
        if node_type == "ResearchProject":
            return one_of(rng, research_titles, "Research Project")
        if node_type == "Publication":
            return one_of(rng, publication_titles, "Publication")
        if node_type == "Internship":
            return one_of(rng, ["Backend Engineering Intern", "Data Engineering Intern", "ML Intern"], "Intern")
        if node_type == "Event":
            return (
                f"{one_of(rng, ['Spring', 'Autumn', 'Annual'], 'Annual')} "
                f"{one_of(rng, ['Graph', 'Data', 'AI'], 'Data')} "
                f"{one_of(rng, ['Hackathon', 'Workshop', 'Meetup'], 'Meetup')}"
            )
        if node_type == "Degree":
            return one_of(rng, [x[0] for x in degree_titles if isinstance(x, list) and len(x) >= 1], "Bachelor of Science")
        return f"{node_type} Title {idx}"

    if "abbreviation" in lname:
        if node_type == "Degree":
            abbrs = [x[1] for x in degree_titles if isinstance(x, list) and len(x) >= 2]
            return one_of(rng, abbrs, "BSc")
        return "".join([c for c in node_type.upper() if c.isalpha()][:4]) or "ABBR"

    if "type" in lname:
        if node_type == "Event":
            return one_of(rng, event_types, "seminar")
        if node_type == "Sponsor":
            return one_of(rng, sponsor_types, "company")
        if node_type == "Publication":
            return one_of(rng, publication_types, "conference-paper")
        if node_type == "Application":
            return one_of(rng, application_types, "scholarship")
        if node_type == "Exam":
            return one_of(rng, exam_types, "final")
        return "standard"

    if "level" in lname:
        return one_of(rng, levels, "intermediate")

    if "language" in lname:
        return one_of(rng, languages, "English")

    if "status" in lname:
        return one_of(rng, statuses, "active")

    if "role" in lname:
        return one_of(rng, roles, "member")

    if "category" in lname:
        return one_of(rng, categories, "academic")

    if "currency" in lname:
        return one_of(rng, currencies, "EUR")

    if "roomnumber" in lname:
        return str(rng.randint(100, 599))

    if "buildingcode" in lname or lname == "code":
        return f"{one_of(rng, building_prefixes, 'A')}-{rng.randint(1, 30)}"

    if "address" in lname:
        return f"{rng.randint(1, 120)} {one_of(rng, street_names, 'Main St')}, {one_of(rng, cities, 'Prague')}"

    if "dayofweek" in lname:
        return one_of(rng, days, "Monday")

    if "starttime" in lname:
        return one_of(rng, start_times, "09:00")

    if "endtime" in lname:
        return one_of(rng, end_times, "10:30")

    if "doi" in lname:
        return f"10.{rng.randint(1000, 9999)}/{rng.getrandbits(32):08x}"

    if "phone" in lname:
        return f"+420{rng.randint(100000000, 999999999)}"

    if "focusarea" in lname:
        return one_of(rng, topics, "Graph Databases")

    if "researcharea" in lname:
        return one_of(rng, topics, "Graph Databases")

    if "industry" in lname:
        return one_of(rng, ["Software", "Cloud", "Security", "Consulting", "Research"], "Software")

    if "deliverymode" in lname:
        return one_of(rng, ["in-person", "hybrid", "online"], "in-person")

    if "recurrence" in lname:
        return one_of(rng, ["weekly", "biweekly", "once"], "weekly")

    if "description" in lname:
        return f"Description for {node_type or relation_type} {idx}"

    if sample is not None:
        return sample

    return f"{node_type or relation_type}_{name}_{idx}"


def build_node_instance(
    rng: random.Random,
    node_schema: Dict[str, Any],
    sample_data: Dict[str, Any],
    idx: int,
    routing_layout: BackendLayout,
    placement_strategy: str,
    source_override: str | None,
) -> Dict[str, Any]:
    node_type = node_schema["name"]
    properties = {}

    for prop in node_schema.get("properties", []):
        properties[prop["name"]] = generate_value_for_property(
            rng, prop, sample_data, node_type=node_type, idx=idx
        )

    if placement_strategy == "schema":
        configured_source = source_override or node_schema.get("source")
        if not configured_source:
            raise ValueError(f"Node type {node_type} has no schema source")
        source_name = routing_layout.source_for_code(routing_layout.code_for(configured_source))
        node_id = make_encoded_id(rng, source_name, "NODE", routing_layout)
    else:
        node_id, source_name = make_routed_id(rng, "NODE", routing_layout)

    return {
        "uuid": node_id,
        "type": node_type,
        "source": source_name,
        "properties": properties,
    }


def build_relation_instance(
    rng: random.Random,
    rel_schema: Dict[str, Any],
    sample_data: Dict[str, Any],
    idx: int,
    from_uuid: int,
    to_uuid: int,
    routing_layout: BackendLayout,
    placement_strategy: str,
    source_override: str | None,
) -> Dict[str, Any]:
    rel_type = rel_schema["name"]
    properties = {}

    for prop in rel_schema.get("properties", []):
        properties[prop["name"]] = generate_value_for_property(
            rng, prop, sample_data, relation_type=rel_type, idx=idx
        )

    if placement_strategy == "schema":
        configured_source = source_override or rel_schema.get("source")
        if not configured_source:
            raise ValueError(f"Relationship type {rel_type} has no schema source")
        source_name = routing_layout.source_for_code(routing_layout.code_for(configured_source))
        rel_id = make_encoded_id(rng, source_name, "REL", routing_layout)
    else:
        rel_id, source_name = make_routed_id(rng, "REL", routing_layout)

    return {
        "uuid": rel_id,
        "type": rel_type,
        "source": source_name,
        "from": from_uuid,
        "to": to_uuid,
        "properties": properties,
    }


def choose_distinct_pairs(
    rng: random.Random,
    from_nodes: List[Dict[str, Any]],
    to_nodes: List[Dict[str, Any]],
    count: int,
    relation_name: str,
) -> List[Tuple[int, int]]:
    max_unique = len(from_nodes) * len(to_nodes)
    target = min(count, max_unique)

    used = set()
    pairs: List[Tuple[int, int]] = []

    if target == 0:
        return pairs

    attempts = 0
    max_attempts = max(1000, target * 20)

    while len(pairs) < target and attempts < max_attempts:
        f = rng.choice(from_nodes)["uuid"]
        t = rng.choice(to_nodes)["uuid"]
        key = (relation_name, f, t)
        if key not in used:
            used.add(key)
            pairs.append((f, t))
        attempts += 1

    if len(pairs) < target:
        for fn in from_nodes:
            for tn in to_nodes:
                key = (relation_name, fn["uuid"], tn["uuid"])
                if key not in used:
                    used.add(key)
                    pairs.append((fn["uuid"], tn["uuid"]))
                    if len(pairs) >= target:
                        return pairs

    return pairs


def generate_dataset(
    schema: Dict[str, Any],
    config: Dict[str, Any],
    sample_data: Dict[str, Any],
    profile_name: str,
    source_override: str = None,
    storage_backends: List[str] | None = None,
    placement_strategy: str = "schema",
) -> Dict[str, Any]:
    if "profiles" not in config or profile_name not in config["profiles"]:
        raise ValueError(f"Profile '{profile_name}' not found in config")

    profile = config["profiles"][profile_name]
    rng = random.Random(profile.get("seed", 42))

    node_counts = profile.get("nodeCounts", {})
    rel_counts = profile.get("relationshipCounts", {})

    if placement_strategy not in {"schema", "identity-hash"}:
        raise ValueError(f"Unsupported placement strategy: {placement_strategy}")

    schema_nodes = {n["name"]: n for n in schema.get("nodeTypes", [])}
    schema_rels = {r["name"]: r for r in schema.get("relations", [])}
    schema_sources = {
        str(entry["source"])
        for entry in [*schema_nodes.values(), *schema_rels.values()]
        if entry.get("source")
    }

    if source_override:
        configured_backends = [source_override]
    elif storage_backends:
        configured_backends = storage_backends
    elif placement_strategy == "identity-hash":
        configured_backends = profile.get(
            "storageBackends",
            config.get(
                "storageBackends",
                schema.get("idRouting", schema.get("storageBackends")),
            ),
        )
        if not configured_backends:
            configured_backends = (
                list(BackendLayout.from_sources(schema_sources).backends)
                if schema_sources
                else list(DEFAULT_ROUTING_BACKENDS)
            )
    else:
        configured_backends = schema.get("idRouting") or schema.get("storageBackends")
        if not configured_backends:
            if not schema_sources:
                raise ValueError("Schema placement requires source fields on node/relation types")
            configured_backends = list(BackendLayout.from_sources(schema_sources).backends)

    routing_layout = BackendLayout(tuple(configured_backends))
    for source in schema_sources:
        routing_layout.code_for(source)

    generated_nodes_by_type: Dict[str, List[Dict[str, Any]]] = {}
    all_nodes: List[Dict[str, Any]] = []

    for node_type, count in node_counts.items():
        if node_type not in schema_nodes:
            continue

        instances = [
            build_node_instance(
                rng,
                schema_nodes[node_type],
                sample_data,
                i + 1,
                routing_layout,
                placement_strategy,
                source_override,
            )
            for i in range(count)
        ]
        generated_nodes_by_type[node_type] = instances
        all_nodes.extend(instances)

    all_relationships: List[Dict[str, Any]] = []

    for rel_name, requested_count in rel_counts.items():
        if rel_name not in schema_rels:
            continue

        rel_schema = schema_rels[rel_name]
        from_type = rel_schema["from"]
        to_type = rel_schema["to"]

        from_nodes = generated_nodes_by_type.get(from_type, [])
        to_nodes = generated_nodes_by_type.get(to_type, [])

        pairs = choose_distinct_pairs(rng, from_nodes, to_nodes, requested_count, rel_name)

        for idx, (from_uuid, to_uuid) in enumerate(pairs, start=1):
            rel_instance = build_relation_instance(
                rng,
                rel_schema,
                sample_data,
                idx,
                from_uuid,
                to_uuid,
                routing_layout,
                placement_strategy,
                source_override,
            )
            all_relationships.append(rel_instance)

    return {
        "domain": schema.get("domain", "Generated Domain"),
        "profile": profile_name,
        "seed": profile.get("seed", 42),
        "generatedAt": datetime.now(UTC).isoformat().replace("+00:00", "Z"),
        "idRoutingStrategy": (
            "schema-type-placement-v1"
            if placement_strategy == "schema"
            else "seeded-random-v1"
        ),
        **routing_layout.metadata(),
        "counts": {
            "nodes": len(all_nodes),
            "relationships": len(all_relationships),
        },
        "nodes": all_nodes,
        "relationships": all_relationships,
    }


def print_decode_examples(dataset: Dict[str, Any]) -> None:
    routing_layout = BackendLayout(tuple(dataset["idRouting"]))
    print("\nDecode examples:")
    if dataset["nodes"]:
        node_id = dataset["nodes"][0]["uuid"]
        print(
            f"  Node ID {node_id} -> "
            f"source={decode_source_from_id(node_id, routing_layout)}"
        )
    if dataset["relationships"]:
        relationship_id = dataset["relationships"][0]["uuid"]
        print(
            f"  Relationship ID {relationship_id} -> "
            f"source={decode_source_from_id(relationship_id, routing_layout)}"
        )


def main():
    parser = argparse.ArgumentParser(
        description=(
            "Generate concrete graph JSON from schema template, size config, and sample data pools. "
            "Each node and relationship gets a numeric uint64 ID with backend routing in its most-significant bits."
        )
    )
    parser.add_argument("schema", help="Path to schema template JSON")
    parser.add_argument("config", help="Path to generation config JSON")
    parser.add_argument("sample_data", help="Path to sample data JSON")
    parser.add_argument("-p", "--profile", choices=["small", "medium", "large"], help="Generation profile")
    parser.add_argument("-s", "--seed", type=int, help="Override seed from config")
    parser.add_argument("--source", help="Legacy single-backend override")
    parser.add_argument(
        "--placement-strategy",
        choices=["schema", "identity-hash"],
        default="schema",
        help="Use schema type sources by default, or distribute individual objects by hash.",
    )
    parser.add_argument(
        "--storage-backends",
        nargs="+",
        choices=["Neo4J", "ArangoDB", "PostgreSQL", "GPU"],
        default=None,
        help="Optional ordered backend layout encoded in ID MSBs; otherwise derive it from the schema.",
    )
    parser.add_argument("-o", "--output", default="generated_university.json", help="Output JSON file")
    args = parser.parse_args()

    schema = load_json(args.schema)
    config = load_json(args.config)
    sample_data = load_json(args.sample_data)

    profile_name = args.profile or config.get("defaultProfile", "small")

    if profile_name not in config.get("profiles", {}):
        raise ValueError(f"Profile '{profile_name}' not found in config")

    if args.seed is not None:
        config["profiles"][profile_name]["seed"] = args.seed

    print(f"Source: {args.source}")

    dataset = generate_dataset(
        schema=schema,
        config=config,
        sample_data=sample_data,
        profile_name=profile_name,
        source_override=args.source,
        storage_backends=args.storage_backends,
        placement_strategy=args.placement_strategy,
    )

    with open(args.output, "w", encoding="utf-8") as f:
        json.dump(dataset, f, indent=2, ensure_ascii=False)

    print(f"Generated profile: {profile_name}")
    print(f"Nodes: {dataset['counts']['nodes']}")
    print(f"Relationships: {dataset['counts']['relationships']}")
    print(f"Saved to: {args.output}")
    print_decode_examples(dataset)


if __name__ == "__main__":
    main()