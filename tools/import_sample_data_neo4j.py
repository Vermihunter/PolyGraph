from neo4j import GraphDatabase

URI = "bolt://localhost:7687"
USERNAME = "neo4j"
PASSWORD = "password"  

driver = GraphDatabase.driver(URI, auth=(USERNAME, PASSWORD))

def clear_database(tx):
    tx.run("MATCH (n) DETACH DELETE n")

def create_sample_data(tx):
    query = """
    CREATE (keanu:Person {name: 'Keanu Reeves', born: 1964})
    CREATE (laurence:Person {name: 'Laurence Fishburne', born: 1961})
    CREATE (carrie:Person {name: 'Carrie-Anne Moss', born: 1967})

    CREATE (matrix:Movie {title: 'The Matrix', released: 1999})
    CREATE (matrix2:Movie {title: 'The Matrix Reloaded', released: 2003})

    CREATE (keanu)-[:ACTED_IN {role: 'Neo'}]->(matrix)
    CREATE (laurence)-[:ACTED_IN {role: 'Morpheus'}]->(matrix)
    CREATE (carrie)-[:ACTED_IN {role: 'Trinity'}]->(matrix)

    CREATE (keanu)-[:ACTED_IN {role: 'Neo'}]->(matrix2)
    CREATE (laurence)-[:ACTED_IN {role: 'Morpheus'}]->(matrix2)
    CREATE (carrie)-[:ACTED_IN {role: 'Trinity'}]->(matrix2)
    """
    tx.run(query)
    
[
    {
        
        "n1": {"type": "", "properties": []}, # Node 1
        "e": {"type": "","properties": []}, # Edge
        "n2": {"type": "","properties": []}  # Node 2
    },
]

def main():
    with driver.session(database="neo4j") as session:
        #print("Clearing database...")
        #session.execute_write(clear_database)

        print("Creating sample data...")
        session.execute_write(create_sample_data)

    print("Done!")

if __name__ == "__main__":
    main()
    driver.close()