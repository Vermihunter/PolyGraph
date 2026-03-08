import json
import random
import argparse
import uuid

sizes = {
    "small": {
        "Person": 80,
        "Company": 20,
        "Skill": 40,
        "Technology": 40,
        "Project": 50,
        "City": 10,
        "University": 10
    },

    "medium": {
        "Person": 800,
        "Company": 120,
        "Skill": 120,
        "Technology": 120,
        "Project": 200,
        "City": 40,
        "University": 40
    },

    "large": {
        "Person": 4000,
        "Company": 500,
        "Skill": 400,
        "Technology": 400,
        "Project": 1000,
        "City": 120,
        "University": 120
    }
}


def random_id(prefix):
    return f"{prefix}_{uuid.uuid4().hex[:10]}"


def generate_nodes(template, scale):

    nodes = {}

    for label, count in scale.items():

        nodes[label] = []

        for i in range(count):

            node = {"id": random_id(label)}

            if label == "Person":

                node["name"] = random.choice(
                    template["node_types"]["Person"]["properties"]["name_pool"]
                )

                node["age"] = random.randint(
                    template["node_types"]["Person"]["properties"]["age_range"][0],
                    template["node_types"]["Person"]["properties"]["age_range"][1]
                )

            else:

                names = template["node_types"][label]["names"]
                node["name"] = random.choice(names) + "_" + str(i)

            nodes[label].append(node)

    return nodes


def generate_relationships(nodes):

    rels = []

    persons = nodes["Person"]
    companies = nodes["Company"]
    skills = nodes["Skill"]
    techs = nodes["Technology"]
    projects = nodes["Project"]
    cities = nodes["City"]
    universities = nodes["University"]

    for p in persons:

        rels.append((p["id"], random.choice(companies)["id"], "WORKS_AT"))

        rels.append((p["id"], random.choice(cities)["id"], "LIVES_IN"))

        rels.append((p["id"], random.choice(universities)["id"], "STUDIED_AT"))

        for _ in range(random.randint(2,6)):
            rels.append((p["id"], random.choice(skills)["id"], "HAS_SKILL"))

        for _ in range(random.randint(1,4)):
            rels.append((p["id"], random.choice(projects)["id"], "WORKED_ON"))

    for pr in projects:

        for _ in range(random.randint(2,5)):
            rels.append((pr["id"], random.choice(techs)["id"], "USES_TECH"))

    for _ in range(len(persons) * 10):

        p1 = random.choice(persons)
        p2 = random.choice(persons)

        if p1["id"] != p2["id"]:

            rels.append((p1["id"], p2["id"], "KNOWS"))

    return rels


def main():

    parser = argparse.ArgumentParser()
    parser.add_argument("--size", choices=["small","medium","large"], required=True)
    parser.add_argument("--json", required=True)

    args = parser.parse_args()

    template = json.load(open(args.json))

    scale = sizes[args.size]

    nodes = generate_nodes(template, scale)

    relationships = generate_relationships(nodes)

    output = {
        "nodes": nodes,
        "relationships": relationships
    }

    with open(f"graph_{args.size}.json","w") as f:

        json.dump(output, f, indent=2)

    print("Nodes:", sum(len(v) for v in nodes.values()))
    print("Relationships:", len(relationships))


if __name__ == "__main__":
    main()