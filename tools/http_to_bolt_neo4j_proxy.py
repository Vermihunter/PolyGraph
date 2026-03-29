import os
from dotenv import load_dotenv
from flask import Flask, request, jsonify
from neo4j import GraphDatabase
from neo4j.graph import Node, Relationship, Path

load_dotenv(".cloud-neo.env")

URI = os.getenv("NEO4J_URI")
USER = os.getenv("NEO4J_USERNAME")
PASSWORD = os.getenv("NEO4J_PASSWORD")
DATABASE = os.getenv("NEO4J_DATABASE")


def serialize(value):
    if isinstance(value, Node):
        return {
            "id": value.element_id,
            "labels": list(value.labels),
            "properties": dict(value)
        }
    elif isinstance(value, Relationship):
        return {
            "id": value.element_id,
            "type": value.type,
            "start": value.start_node.element_id,
            "end": value.end_node.element_id,
            "properties": dict(value)
        }
    elif isinstance(value, Path):
        return {
            "nodes": [serialize(n) for n in value.nodes],
            "relationships": [serialize(r) for r in value.relationships]
        }
    elif isinstance(value, list):
        return [serialize(v) for v in value]
    elif isinstance(value, dict):
        return {k: serialize(v) for k, v in value.items()}
    else:
        return value

driver = GraphDatabase.driver(URI, auth=(USER, PASSWORD))

app = Flask(__name__)


@app.route("/db/cloud-neo4j/tx/commit", methods=["POST"])
def commit():
    body = request.json

    statements = body.get("statements", [])
    results = []
    
    print(f"Got request - {body}")

    with driver.session(database=DATABASE) as session:
        for stmt in statements:
            query = stmt["statement"]
            params = stmt.get("parameters", {})

            #print("Sending query")
            res = session.run(query, **params)
            #print(f"Got response: {res}")

            data = []
            for record in res:
                #data.append({"row": list(record.values())})
                
                data.append({
                    "row": [serialize(v) for v in record.values()]
                })

            #print(f"Got response: {data}")
            results.append({"data": data})
    
    #print(f"Sending response: {results}")

    return jsonify({
        "results": results,
        "errors": []
    })

if __name__ == "__main__":
    app.run(port=9696)