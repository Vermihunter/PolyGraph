from neo4j import GraphDatabase
import random

URI = "bolt://localhost:7688"
USERNAME = ""
PASSWORD = ""  # Memgraph usually doesn't require auth by default

NUM_NODES = 50
NUM_EDGES = 100


def populate(tx):
    # Create nodes
    for i in range(NUM_NODES):
        tx.run(
            "CREATE (:Person {id: $id, name: $name})",
            id=i,
            name=f"Person_{i}"
        )

    # Create edges
    for _ in range(NUM_EDGES):
        a = random.randint(0, NUM_NODES - 1)
        b = random.randint(0, NUM_NODES - 1)

        res = tx.run("""
        MATCH (p1:Person {id: $a}), (p2:Person {id: $b})
        CREATE (p1)-[:KNOWS]->(p2)
        """, a=a, b=b)
        
        print(res)


def main():
    driver = GraphDatabase.driver(URI, auth=(USERNAME, PASSWORD))

    with driver.session() as session:
        session.execute_write(populate)

    driver.close()
    print("Database populated with 50 nodes and 100 edges.")


if __name__ == "__main__":
    main()