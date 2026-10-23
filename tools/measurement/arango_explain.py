#!/usr/bin/env python3
"""Verify that PolyGraph's hot ArangoDB BFS query uses the edge index."""
from __future__ import annotations

import argparse
import base64
import json
import os
import sys
import urllib.error
import urllib.parse
import urllib.request
from typing import Any


def _request_json(
    url: str,
    payload: dict[str, Any],
    *,
    username: str | None,
    password: str | None,
) -> dict[str, Any]:
    data = json.dumps(payload, separators=(",", ":")).encode("utf-8")
    headers = {"Content-Type": "application/json", "Accept": "application/json"}
    if username is not None:
        credentials = f"{username}:{password or ''}".encode("utf-8")
        headers["Authorization"] = "Basic " + base64.b64encode(credentials).decode("ascii")

    request = urllib.request.Request(url, data=data, headers=headers, method="POST")
    try:
        with urllib.request.urlopen(request, timeout=30.0) as response:
            raw = response.read()
    except urllib.error.HTTPError as error:
        raw = error.read()
        message = raw.decode("utf-8", errors="replace")
        raise RuntimeError(f"ArangoDB returned HTTP {error.code}: {message}") from error
    return json.loads(raw.decode("utf-8"))


def _index_description(node: dict[str, Any]) -> str:
    indexes = node.get("indexes")
    if not isinstance(indexes, list):
        return ""
    parts: list[str] = []
    for index in indexes:
        if not isinstance(index, dict):
            continue
        index_type = str(index.get("type", "?"))
        fields = index.get("fields", [])
        if isinstance(fields, list):
            fields_text = ",".join(str(field) for field in fields)
        else:
            fields_text = str(fields)
        name = str(index.get("name", ""))
        parts.append(f"{index_type}:{fields_text}" + (f" ({name})" if name else ""))
    return "; ".join(parts)


def main() -> int:
    parser = argparse.ArgumentParser(
        description=(
            "Run ArangoDB EXPLAIN for the same edge-index lookup shape used by "
            "PolyGraph BFS and fail if no IndexNode is selected."
        )
    )
    parser.add_argument("--url", default="http://127.0.0.1:8529")
    parser.add_argument("--database", default="_system")
    parser.add_argument("--username", default=os.environ.get("ARANGO_USERNAME", "root"))
    parser.add_argument(
        "--password",
        default=os.environ.get("ARANGO_ROOT_PASSWORD") or os.environ.get("ARANGO_PASSWORD"),
        help="Defaults to ARANGO_ROOT_PASSWORD/ARANGO_PASSWORD.",
    )
    parser.add_argument("--edge-collection", required=True)
    parser.add_argument("--vertex-collection", required=True)
    parser.add_argument(
        "--vertex-id",
        action="append",
        required=True,
        help="Document _key/global-id payload. Repeat to explain a larger frontier.",
    )
    parser.add_argument("--target-type", default="Unknown")
    parser.add_argument("--direction", choices=["outbound", "inbound"], default="outbound")
    parser.add_argument(
        "--return-source",
        action="store_true",
        help="Use the predecessor-preserving 3-column query instead of candidate-only BFS.",
    )
    args = parser.parse_args()

    source_endpoint = "edge._from" if args.direction == "outbound" else "edge._to"
    neighbor_endpoint = "edge._to" if args.direction == "outbound" else "edge._from"
    if args.return_source:
        query = (
            "FOR edge IN @@edgeCollection "
            f"FILTER {source_endpoint} IN @frontier "
            f"RETURN [{source_endpoint}, {neighbor_endpoint}, @targetType]"
        )
    else:
        query = (
            "FOR edge IN @@edgeCollection "
            f"FILTER {source_endpoint} IN @frontier "
            f"RETURN [{neighbor_endpoint}, @targetType]"
        )

    frontier = [
        f"{args.vertex_collection}/{value.split('/', 1)[-1]}"
        for value in args.vertex_id
    ]
    payload = {
        "query": query,
        "bindVars": {
            "@edgeCollection": args.edge_collection,
            "frontier": frontier,
            "targetType": args.target_type,
        },
    }

    database = urllib.parse.quote(args.database, safe="")
    endpoint = args.url.rstrip("/") + f"/_db/{database}/_api/explain"
    result = _request_json(
        endpoint,
        payload,
        username=args.username,
        password=args.password,
    )

    plan = result.get("plan", {})
    nodes = plan.get("nodes", [])
    if not isinstance(nodes, list):
        print(json.dumps(result, indent=2))
        raise RuntimeError("ArangoDB EXPLAIN response did not contain plan.nodes")

    print("AQL:")
    print("  " + query)
    print(f"Frontier size: {len(frontier)}")
    print("\nExecution plan:")
    has_index = False
    has_collection_scan = False
    edge_index = False

    for node in nodes:
        if not isinstance(node, dict):
            continue
        node_type = str(node.get("type", "?"))
        collection = node.get("collection")
        collection_name = ""
        if isinstance(collection, str):
            collection_name = collection
        elif isinstance(collection, dict):
            collection_name = str(collection.get("name", ""))
        index_text = _index_description(node)

        detail = node_type
        if collection_name:
            detail += f" collection={collection_name}"
        if index_text:
            detail += f" indexes=[{index_text}]"
        print("  " + detail)

        if node_type == "IndexNode":
            has_index = True
            indexes = node.get("indexes", [])
            if isinstance(indexes, list):
                for index in indexes:
                    if not isinstance(index, dict):
                        continue
                    fields = index.get("fields", [])
                    if (
                        index.get("type") == "edge"
                        or source_endpoint.removeprefix("edge.") in fields
                        or source_endpoint in fields
                    ):
                        edge_index = True
        if node_type == "EnumerateCollectionNode" and (
            not collection_name or collection_name == args.edge_collection
        ):
            has_collection_scan = True

    print()
    if has_collection_scan:
        print("FAIL: the plan contains an EnumerateCollectionNode for the edge lookup.")
        return 2
    if not has_index:
        print("FAIL: no IndexNode is present in the execution plan.")
        return 2
    if edge_index:
        print("OK: ArangoDB selected an edge/index lookup for the BFS query.")
    else:
        print("OK: an IndexNode is present; inspect the index details above to confirm _from/_to.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
