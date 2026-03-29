#!/usr/bin/env python3

import json
import argparse
import textwrap
from graphviz import Digraph

# python3 visualize-domain.py -o domain -f svg --view ../../storage_layer/sample-university-domain.graph-shape.json

def wrap_text(text: str, width: int = 28) -> str:
    if not text:
        return ""
    return "\\n".join(textwrap.wrap(text, width=width))


def format_properties(properties):
    if not properties:
        return "properties: none"

    lines = []
    for prop in properties:
        name = prop.get("name", "unknown")
        ptype = prop.get("type", "unknown")
        sample = prop.get("sampleValue", "")
        lines.append(f"{name}: {ptype} = {sample}")
    return "\\n".join(lines)


def build_node_label(node):
    name = node.get("name", "UnknownNode")
    meaning = wrap_text(node.get("meaning", ""), 32)
    source = node.get("source", "Unknown")
    properties = format_properties(node.get("properties", []))

    label = (
        f"<<TABLE BORDER='1' CELLBORDER='0' CELLSPACING='0' CELLPADDING='6'>"
        f"<TR><TD><B>{name}</B></TD></TR>"
        f"<TR><TD ALIGN='LEFT'><FONT POINT-SIZE='10'>{meaning}</FONT></TD></TR>"
        f"<TR><TD ALIGN='LEFT'><FONT POINT-SIZE='10'><I>source: {source}</I></FONT></TD></TR>"
        f"<TR><TD ALIGN='LEFT'><FONT POINT-SIZE='9'>{properties}</FONT></TD></TR>"
        f"</TABLE>>"
    )
    return label


def build_edge_label(rel):
    name = rel.get("name", "UNKNOWN_REL")
    meaning = wrap_text(rel.get("meaning", ""), 26)
    source = rel.get("source", "Unknown")
    properties = format_properties(rel.get("properties", []))

    label = f"{name}\\n{meaning}\\nsource: {source}\\n{properties}"
    return label


def generate_graph(schema, graph_name="UniversitySchema"):
    dot = Digraph(name=graph_name, format="png")
    dot.attr(rankdir="LR", splines="true", overlap="false")
    dot.attr("graph", fontsize="12", fontname="Helvetica")
    dot.attr("node", shape="plaintext", fontname="Helvetica")
    dot.attr("edge", fontname="Helvetica", fontsize="10")

    node_types = schema.get("nodeTypes", [])
    relations = schema.get("relations", [])

    # Create nodes
    node_names = set()
    for node in node_types:
        node_name = node.get("name")
        if not node_name:
            continue
        node_names.add(node_name)
        dot.node(node_name, label=build_node_label(node))

    # Add placeholder nodes if a relation references a node not present in nodeTypes
    for rel in relations:
        from_node = rel.get("from")
        to_node = rel.get("to")

        for missing_node in [from_node, to_node]:
            if missing_node and missing_node not in node_names:
                dot.node(
                    missing_node,
                    label=f"<<TABLE BORDER='1' CELLBORDER='0' CELLSPACING='0' CELLPADDING='6'>"
                          f"<TR><TD><B>{missing_node}</B></TD></TR>"
                          f"<TR><TD><FONT POINT-SIZE='10'>Placeholder node</FONT></TD></TR>"
                          f"</TABLE>>"
                )
                node_names.add(missing_node)

    # Create edges
    for rel in relations:
        from_node = rel.get("from")
        to_node = rel.get("to")
        if not from_node or not to_node:
            continue

        dot.edge(
            from_node,
            to_node,
            label=build_edge_label(rel)
        )

    return dot


def main():
    parser = argparse.ArgumentParser(
        description="Read a graph schema JSON file and generate a Graphviz diagram."
    )
    parser.add_argument(
        "input_json",
        help="Path to input JSON schema file"
    )
    parser.add_argument(
        "-o", "--output",
        default="graph_schema",
        help="Output file name without extension (default: graph_schema)"
    )
    parser.add_argument(
        "-f", "--format",
        default="png",
        choices=["png", "pdf", "svg", "jpg"],
        help="Output render format (default: png)"
    )
    parser.add_argument(
        "--view",
        action="store_true",
        help="Open the rendered file after generation"
    )


    args = parser.parse_args()

    with open(args.input_json, "r", encoding="utf-8") as f:
        schema = json.load(f)

    graph = generate_graph(schema, graph_name=schema.get("domain", "GraphSchema"))
    graph.format = args.format
    output_path = graph.render(filename=args.output, cleanup=True, view=args.view)

    print(f"Graph generated: {output_path}")


if __name__ == "__main__":
    main()