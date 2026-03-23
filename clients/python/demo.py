"""Inserts sample users, creates an index, and shows explain output for the same query as a
collection scan and as an index scan, side by side.

Usage: python3 demo.py [host] [port]   (start docstore_server first)
"""

import random
import sys

from docstore_client import DocStoreClient, DocStoreError

CITIES = ["Pune", "Delhi", "Goa", "Mumbai", "Chennai"]


def summary(explain):
    plan = explain["queryPlanner"]["winningPlan"]
    stages = plan["stage"]
    if "inputStage" in plan:
        ix = plan["inputStage"]
        stages += f" <- {ix['stage']} {ix['indexName']} {ix['indexBounds']}"
    stats = explain["executionStats"]
    return [
        ("plan", stages),
        ("nReturned", stats["nReturned"]),
        ("totalKeysExamined", stats["totalKeysExamined"]),
        ("totalDocsExamined", stats["totalDocsExamined"]),
    ]


def main():
    host = sys.argv[1] if len(sys.argv) > 1 else "127.0.0.1"
    port = int(sys.argv[2]) if len(sys.argv) > 2 else 7070
    rng = random.Random(42)
    with DocStoreClient(host, port) as db:
        try:
            db.drop_collection("demo_users")
        except DocStoreError:
            pass  # first run: nothing to drop
        for i in range(5000):
            db.insert("demo_users", {"_id": f"user{i:05d}", "age": rng.randint(18, 80),
                                     "city": rng.choice(CITIES)})
        query = {"age": {"$gte": 30, "$lt": 32}, "city": "Pune"}
        before = db.explain("demo_users", query)
        db.create_index("demo_users", "age")
        after = db.explain("demo_users", query)

        print(f"query: {query}  (5000 documents)\n")
        print(f"{'':20} {'without index':40} with index on age")
        for (label, a), (_, b) in zip(summary(before), summary(after)):
            print(f"{label:20} {str(a):40} {b}")
        print(f"\nindexes on demo_users: {db.list_indexes('demo_users')}")


if __name__ == "__main__":
    main()
