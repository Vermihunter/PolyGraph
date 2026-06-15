import json
import csv
import argparse
from collections import deque

"""
python3 bfs.py \
  --input-json ../generators/large-university.json \
  --output-csv bfs_output.csv \
  --start-uuid 11a96cb7-c043-4aa7-aa35-2297e9be7f06
"""


class Graph:
    def __init__(self):
        self.nodes = {}   # uuid -> full node data
        self.adj = {}     # uuid -> list of neighbor uuids

    def add_node(self, node):
        node_uuid = node["uuid"]
        self.nodes[node_uuid] = node
        if node_uuid not in self.adj:
            self.adj[node_uuid] = []

    def add_edge(self, from_uuid, to_uuid, undirected=True):
        if from_uuid not in self.adj:
            self.adj[from_uuid] = []
        if to_uuid not in self.adj:
            self.adj[to_uuid] = []

        self.adj[from_uuid].append(to_uuid)
        if undirected:
            self.adj[to_uuid].append(from_uuid)

    def bfs_depths(self, start_uuid, max_depth=None):
        if start_uuid not in self.adj:
            raise ValueError(f"Start node {start_uuid} not found in graph.")

        visited = {start_uuid}
        depths = {start_uuid: 0}
        queue = deque([start_uuid])

        while queue:
            current = queue.popleft()

            if max_depth is not None and depths[current] >= max_depth:
                continue

            for neighbor in self.adj[current]:
                if neighbor not in visited:
                    visited.add(neighbor)
                    depths[neighbor] = depths[current] + 1
                    queue.append(neighbor)

        return depths


def load_graph_from_json(json_path, undirected=True):
    with open(json_path, "r", encoding="utf-8") as f:
        data = json.load(f)

    graph = Graph()

    for node in data.get("nodes", []):
        graph.add_node(node)

    for rel in data.get("relationships", []):
        from_uuid = rel["from"]
        to_uuid = rel["to"]
        graph.add_edge(from_uuid, to_uuid, undirected=undirected)

    return graph


def save_depths_to_csv(depths, csv_path):
    with open(csv_path, "w", newline="", encoding="utf-8") as f:
        writer = csv.writer(f, delimiter=';')
        writer.writerow(["vertex_id", "distance"])

        for node_uuid, depth in depths:
            writer.writerow([node_uuid, depth])


def parse_args():
    parser = argparse.ArgumentParser(
        description="Load a graph from JSON, run BFS from a start UUID, and save depths to CSV."
    )

    parser.add_argument(
        "--input-json",
        required=True,
        help="Path to the input JSON file containing nodes and relationships."
    )
    parser.add_argument(
        "--output-csv",
        required=True,
        help="Path to the output CSV file."
    )
    parser.add_argument(
        "--start-uuid",
        required=True,
        help="UUID of the node where BFS should start."
    )
    parser.add_argument(
        "--directed",
        action="store_true",
        help="Treat relationships as directed. By default, graph is undirected."
    )

    return parser.parse_args()


def main():
    args = parse_args()

    graph = load_graph_from_json(
        json_path=args.input_json,
        undirected=not args.directed
    )

    depths = graph.bfs_depths(args.start_uuid, max_depth=7)
    depths = sorted(depths.items(), key=lambda x: (x[1],x[0]))
    #print (depths)
    save_depths_to_csv(depths, args.output_csv)

    print(f"BFS completed from {args.start_uuid}")
    print(f"Results written to {args.output_csv}")


if __name__ == "__main__":
    main()