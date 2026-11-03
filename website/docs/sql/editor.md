---
title: SQL editor and completion
sidebar_position: 1
---

# SQL editor and completion

- **Run**: **Cmd+Return** (Ctrl+Enter) runs the statement under the caret; the result opens in **Results**, the timing and messages in **Output**.
- **Completion** pops up as you type and ranks what fits the context: after `FROM` / `JOIN`, tables; after `alias.`, that table's columns; elsewhere, the columns of the tables in the statement, aliases, keywords and functions. It learns what you use (locally, per connection): tables you often join rank higher.

## JOIN conditions

Right after `JOIN <table> ON`, Lume proposes **the whole condition**:

```sql
SELECT *
FROM shipments s
JOIN order_items oi ON oi.order_id = s.order_id AND oi.line = s.item_line
--                    ^ proposed from the foreign key, both columns of the composite key
```

1. **Foreign keys first**, between the joined table and each table before it, in either direction, with the aliases of your statement and every pair of a composite key.
2. **Then same-named columns** that are a primary key on one side (`p.customer_id = o.customer_id`), labelled "same column name". Generic columns such as `created_at`, and each table's own `id`, are never proposed.

Foreign keys are read once per table, the first time a JOIN needs them.
