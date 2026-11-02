# SPDX-License-Identifier: AGPL-3.0-only
# Deterministic SQLite catalog for the ER diagram tests (docs/diagrams.md, DGM-078).
#   python3 dgm_fixture.py <out.db> [tables]
# Default: 14,000 tables, >= 120,000 columns, >= 30,000 foreign keys. Synthetic names only. Shapes:
# composite keys and FKs, self references, cycles (a <-> b), tables with no FK, "*_id" columns with no
# constraint (DGM-012), unique keys (cardinality one-to-one), a hub referenced by many tables, and a
# small named core (customers / orders / order_items / products / audit_event_history) the UI tests use.
import os, sqlite3, sys

out = sys.argv[1]
N = int(sys.argv[2]) if len(sys.argv) > 2 else 14000
DOMAINS = ["sales", "stock", "billing", "people", "audit", "ship", "claims", "plan", "care", "lab"]
if os.path.exists(out):
    os.remove(out)
db = sqlite3.connect(out)
db.execute("PRAGMA journal_mode = OFF")
db.execute("PRAGMA synchronous = OFF")
seed = [12345]
def rnd(n):
    seed[0] = (seed[0] * 1103515245 + 12345) & 0x7FFFFFFF
    return seed[0] % n

core = [
    "CREATE TABLE customers (id INTEGER PRIMARY KEY, name TEXT NOT NULL, email TEXT UNIQUE, city TEXT, region_id INTEGER)",
    "CREATE TABLE products (id INTEGER PRIMARY KEY, sku TEXT NOT NULL UNIQUE, name TEXT NOT NULL, price NUMERIC)",
    "CREATE TABLE orders (id INTEGER PRIMARY KEY, customer_id INTEGER NOT NULL REFERENCES customers(id), status TEXT, total NUMERIC, created_at TEXT)",
    "CREATE TABLE order_items (order_id INTEGER NOT NULL REFERENCES orders(id), line INTEGER NOT NULL, product_id INTEGER REFERENCES products(id), qty INTEGER, PRIMARY KEY (order_id, line))",
    "CREATE TABLE shipments (id INTEGER PRIMARY KEY, order_id INTEGER NOT NULL, item_line INTEGER NOT NULL, shipped_at TEXT, FOREIGN KEY (order_id, item_line) REFERENCES order_items(order_id, line))",
    "CREATE TABLE customer_profiles (customer_id INTEGER PRIMARY KEY REFERENCES customers(id), bio TEXT)",
    "CREATE TABLE employees (id INTEGER PRIMARY KEY, name TEXT, manager_id INTEGER REFERENCES employees(id))",
    "CREATE TABLE audit_event_history (id INTEGER PRIMARY KEY, actor_id INTEGER, happened_at TEXT, what TEXT)",
    "CREATE TABLE archive_orders (id INTEGER PRIMARY KEY, customer_id INTEGER, total NUMERIC)",
]
for s in core:
    db.execute(s)
names = []
cols_total = 0
fks_total = 4
for i in range(N - len(core)):
    dom = DOMAINS[i % len(DOMAINS)]
    name = "%s_t%05d" % (dom, i)
    cols = ["id INTEGER PRIMARY KEY", "code TEXT NOT NULL", "label TEXT", "amount NUMERIC", "created_at TEXT", "updated_at TEXT", "note TEXT"]
    fks = []
    if i > 10:
        nf = 2 + (1 if rnd(5) == 0 else 0) + (1 if rnd(9) == 0 else 0)
        if i % 211 == 0:
            nf = 0                                   # no FK at all
        for k in range(nf):
            j = rnd(i)                               # an earlier table: a DAG...
            if i % 150 == 7 and k == 0:
                j = 0                                # ...with a hub (table 0 referenced a lot)
            col = "ref%d_id" % k
            cols.append("%s INTEGER%s" % (col, " NOT NULL" if k == 0 else ""))
            fks.append("FOREIGN KEY (%s) REFERENCES %s(id)" % (col, names[j]))
    if i % 97 == 0:
        cols.append("parent_id INTEGER")
        fks.append("FOREIGN KEY (parent_id) REFERENCES %s(id)" % name)   # self reference
    cols.append("legacy_id INTEGER")                                       # an *_id with no constraint
    if i % 13 == 0:
        cols.append("ext_code TEXT UNIQUE")
    db.execute("CREATE TABLE %s (%s)" % (name, ", ".join(cols + fks)))
    names.append(name)
    cols_total += len(cols)
    fks_total += len(fks)
# cycles: SQLite allows forward references, so pairs that reference each other
for c in range(0, 400, 2):
    a, b = "cyc_a%03d" % c, "cyc_b%03d" % c
    db.execute("CREATE TABLE %s (id INTEGER PRIMARY KEY, b_id INTEGER, FOREIGN KEY (b_id) REFERENCES %s(id))" % (a, b))
    db.execute("CREATE TABLE %s (id INTEGER PRIMARY KEY, a_id INTEGER, FOREIGN KEY (a_id) REFERENCES %s(id))" % (b, a))
    fks_total += 2
db.commit()
db.close()
print("%s: %d tables, %d columns, %d foreign keys" % (out, N + 400, cols_total + 40, fks_total))
