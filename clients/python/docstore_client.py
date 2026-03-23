"""Minimal DocStore client (Python 3 standard library only).

The server speaks newline-delimited JSON: one request object per line, one response per line.
Every method returns the response dict; a response with ok == 0 raises DocStoreError.
"""

import json
import socket


class DocStoreError(Exception):
    def __init__(self, code, errmsg):
        super().__init__(f"{code}: {errmsg}")
        self.code = code
        self.errmsg = errmsg


class DocStoreClient:
    def __init__(self, host="127.0.0.1", port=7070, timeout=30.0):
        self._sock = socket.create_connection((host, port), timeout=timeout)
        self._file = self._sock.makefile("rwb")

    def close(self):
        self._file.close()
        self._sock.close()

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()

    def request(self, op, **fields):
        fields["op"] = op
        self._file.write(json.dumps(fields).encode() + b"\n")
        self._file.flush()
        line = self._file.readline()
        if not line:
            raise ConnectionError("server closed the connection")
        response = json.loads(line)
        if response.get("ok") != 1:
            raise DocStoreError(response.get("code"), response.get("errmsg"))
        return response

    # One method per server op.
    def ping(self):
        return self.request("ping")

    def list_collections(self):
        return self.request("listCollections")["collections"]

    def create_collection(self, coll):
        return self.request("createCollection", coll=coll)

    def drop_collection(self, coll):
        return self.request("dropCollection", coll=coll)

    def insert(self, coll, doc, sync=False):
        return self.request("insert", coll=coll, doc=doc, sync=sync)["insertedId"]

    def upsert(self, coll, doc, sync=False):
        return self.request("upsert", coll=coll, doc=doc, sync=sync)

    def update(self, coll, id, update, sync=False):
        return self.request("update", coll=coll, id=id, update=update, sync=sync)

    def delete(self, coll, id, sync=False):
        return self.request("delete", coll=coll, id=id, sync=sync)

    def get(self, coll, id):
        return self.request("get", coll=coll, id=id)["doc"]

    def find(self, coll, filter=None, limit=0):
        return self.request("find", coll=coll, filter=filter or {}, limit=limit)["docs"]

    def explain(self, coll, filter=None):
        return self.request("explain", coll=coll, filter=filter or {})["explain"]

    def create_index(self, coll, field):
        return self.request("createIndex", coll=coll, field=field)

    def drop_index(self, coll, field):
        return self.request("dropIndex", coll=coll, field=field)

    def list_indexes(self, coll):
        return self.request("listIndexes", coll=coll)["indexes"]

    def stats(self):
        return self.request("stats")["stats"]

    def flush(self):
        return self.request("flush")

    def compact(self):
        return self.request("compact")
