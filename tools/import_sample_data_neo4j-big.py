import os
import json
import argparse
from dotenv import load_dotenv
from neo4j import GraphDatabase

load_dotenv(".local-neo.env")

NEO4J_URI = os.getenv("NEO4J_URI")
NEO4J_USERNAME = os.getenv("NEO4J_USERNAME")
NEO4J_PASSWORD = os.getenv("NEO4J_PASSWORD")
NEO4J_DATABASE = os.getenv("NEO4J_DATABASE")

BATCH_SIZE = 1000

driver = GraphDatabase.driver(
    NEO4J_URI,
    auth=(NEO4J_USERNAME, NEO4J_PASSWORD)
)


# ------------------------
# LOAD JSON
# ------------------------

def load_json(path):
    with open(path) as f:
        return json.load(f)


# ------------------------
# BATCH HELPER
# ------------------------

def chunks(lst, size):
    for i in range(0, len(lst), size):
        yield lst[i:i + size]


# ------------------------
# CREATE NODES
# ------------------------

def create_nodes_batch(tx, label, nodes):

    query = f"""
    UNWIND $nodes AS node
    CREATE (n:{label})
    SET n = node
    """

    tx.run(query, nodes=nodes)


# ------------------------
# CREATE RELATIONSHIPS
# ------------------------

def create_relationships_batch(tx, rels):

    # group relationships by type
    rel_groups = {}

    for from_id, to_id, rel_type in rels:
        rel_groups.setdefault(rel_type, []).append((from_id, to_id))

    for rel_type, group in rel_groups.items():

        query = f"""
        UNWIND $rels AS rel
        MATCH (a {{id: rel[0]}})
        MATCH (b {{id: rel[1]}})
        CREATE (a)-[:{rel_type}]->(b)
        """

        tx.run(query, rels=group)

# ------------------------
# LOAD GRAPH
# ------------------------

def load_graph(data):

    nodes = data["nodes"]
    relationships = data["relationships"]

    with driver.session(database=NEO4J_DATABASE) as session:

        print("Loading nodes...")

        for label, node_list in nodes.items():

            for batch in chunks(node_list, BATCH_SIZE):

                session.execute_write(
                    create_nodes_batch,
                    label,
                    batch
                )

        print("Loading relationships...")

        for batch in chunks(relationships, BATCH_SIZE):

            session.execute_write(
                create_relationships_batch,
                batch
            )


# ------------------------
# MAIN
# ------------------------

def main():

    parser = argparse.ArgumentParser()

    parser.add_argument(
        "--file",
        required=True,
        help="JSON graph file"
    )

    args = parser.parse_args()

    data = load_json(args.file)

    load_graph(data)

    print("Graph successfully loaded.")


if __name__ == "__main__":
    main()