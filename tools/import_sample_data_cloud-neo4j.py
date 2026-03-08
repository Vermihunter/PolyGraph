import os
from dotenv import load_dotenv
from neo4j import GraphDatabase
import json
import random

load_dotenv(".cloud-neo.env")


NEO4J_URI = os.getenv("NEO4J_URI")
NEO4J_USERNAME = os.getenv("NEO4J_USERNAME")
NEO4J_PASSWORD = os.getenv("NEO4J_PASSWORD")
NEO4J_DATABASE = os.getenv("NEO4J_DATABASE")
AURA_INSTANCEID="ed758294"

driver = GraphDatabase.driver(NEO4J_URI, auth=(NEO4J_USERNAME, NEO4J_PASSWORD))

def create_node(tx, label, properties):
    query = f"""
    CREATE (n:{label} $props)
    """
    tx.run(query, props=properties)


def create_relationship(tx, from_id, to_id, rel_type):
    query = """
    MATCH (a {id: $from_id})
    MATCH (b {id: $to_id})
    CREATE (a)-[:%s]->(b)
    """ % rel_type

    tx.run(query, from_id=from_id, to_id=to_id)


def load_data(file_path):
    with open(file_path) as f:
        return json.load(f)


def generate_relationships(data):
    persons = data["nodes"]["Person"]
    companies = data["nodes"]["Company"]
    skills = data["nodes"]["Skill"]
    projects = data["nodes"]["Project"]

    relationships = []

    # WORKS_AT
    for p in persons:
        c = random.choice(companies)
        relationships.append((p["id"], c["id"], "WORKS_AT"))

    # HAS_SKILL
    for p in persons:
        for _ in range(2):
            s = random.choice(skills)
            relationships.append((p["id"], s["id"], "HAS_SKILL"))

    # WORKED_ON
    for p in persons:
        pr = random.choice(projects)
        relationships.append((p["id"], pr["id"], "WORKED_ON"))

    # random collaborations
    for _ in range(50):
        p1 = random.choice(persons)
        p2 = random.choice(persons)
        if p1 != p2:
            relationships.append((p1["id"], p2["id"], "KNOWS"))

    return relationships


def main():

    data = load_data("CloudNeo4JNodeData.json")

    relationships = generate_relationships(data)

    with driver.session(database=NEO4J_DATABASE) as session:

        # create nodes
        for label, nodes in data["nodes"].items():
            for node in nodes:
                session.execute_write(create_node, label, node)

        # create relationships
        for from_id, to_id, rel in relationships:
            session.execute_write(create_relationship, from_id, to_id, rel)


if __name__ == "__main__":
    main()