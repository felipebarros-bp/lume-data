---
title: What is sent
---

# What is sent

Each request shows its context in the panel before it goes; **Show the exact payload** shows the text itself. This page lists every kind of content and the consent it needs.

| Content | Sent when |
|---|---|
| Your question and the conversation so far | always (that is the request) |
| The SQL of the query tab or its selection, and the dialect | in a query tab |
| The last error of the tab (message and code) | when asking to fix or explain it |
| The result's column names and types | in a query tab with a result |
| **Database structure**: table and column names, types, keys, comments | only after **Share the structure of** `<connection>`, remembered per connection |
| **Category values**: up to 20 distinct values of short text columns (`status`, `channel`...) | only with **Also share category values**, per connection. Columns that look personal (names, e-mails, documents, phones, addresses) are never sampled |
| **A summary of the current result**: rows, nulls, distinct values, min / max per column, computed locally | only with its own consent |
| **Data values** of a report card | only with **Data values: ON (your risk)**, asked again every session |
| A plan, for plan analysis | see [Execution plans](../sql/execution-plans.md#lume-intelligence): literals masked and names aliased by default |
| A staged Git diff | only after you see it and press **Send**; secret files are left out |

**Rows of your tables are never sent** for SQL help: the AI checks its queries with a test run that reads no rows, and when one fails, only the error text and column names go back to it.

## Large databases

For a database with thousands of tables, Lume does not send the whole structure. It keeps a local index of the catalog (names, types, keys, comments) and, for each question, picks the tables that matter (typically 20 to 40) plus a short summary of the database. The model may ask Lume to search or describe more tables, for at most two rounds. The words your own queries use near each table also help the picking; your queries themselves are not sent.

## Scope per connection

The context panel sets, per connection:

- **Schemas** that may be shared;
- **Always send**: tables always included;
- **Never send**: tables whose structure is never shared;
- **Read structure again** after a schema change.

It also shows the size of the next request ("32 tables, ~9k tokens") with the list. **Settings > Lume Intelligence > Remembered per connection** lists what each connection allows, with **Forget**.

## A read-only login for the AI's queries

When the assistant's SQL runs (report cards, same-result checks), you can make it run as a separate connection whose login can only read: in the consent card, **Run the assistant's queries as (choose one):**. The structure is then read and the queries run as that login, so the database itself refuses writes and its grants limit what the model sees. Lume checks the login's write permissions and warns when it can write. This check helps but is not a proof: permissions granted through roles may be missed.

## Glossary

A glossary of your business terms ("active customer = a customer with an order in the last 90 days") is sent with every request and also helps pick tables. Edit it from the assistant's context or **Settings > Lume Intelligence > Edit glossary...**. It is kept in the workspace folder (or Lume's preferences folder) as `glossary.txt`, one `term = definition` per line.

## Audit

Every send, and every refusal to send, is recorded in an audit log on your computer with its size and decision, never its content.
