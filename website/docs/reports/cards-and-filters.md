---
title: Cards, data and filters
---

# Cards, data and filters

## Where a card's data comes from

A card gets its numbers in one of two ways.

### A metric from the semantic catalog

The semantic catalog is a curated list of **metrics** (for example "revenue" = sum of `orders.total`, with its filters, unit or currency and allowed time grains) over the tables you authorise. It lives in `semantic.json` in Lume's preferences folder (Settings > General shows where). After editing it, **Tools > Reload Semantic Catalog**.

A card built on a metric is pinned to the metric's version, and Lume writes its SQL itself. Pick in the card's properties:

- **Metric**, and for charts and tables a **Dimension** or a **Time grain** (day, week, month, quarter, year);
- for a KPI, a **Comparison**: **No comparison**, **Previous period** or **Same period last year (YoY)**.

Units and currencies are checked: a card cannot mix incompatible ones. Certified metrics are the most reliable source, and the assistant prefers them.

### The card's own SQL

Any card can instead run its own `SELECT`. Editing the card shows its **Query** in an embedded editor with the run status, row count and errors:

- **Apply** (**Cmd+Enter**) checks the query against the read-only rules and test-runs it with no rows read (`WHERE 1=0`) before replacing the card's query.
- On an error, **Ask the AI to fix it** sends the error and the SQL (never rows) to [Lume Intelligence](../intelligence/overview.md).

Shapes the visuals expect: a **Line** or **Bars** card needs exactly two columns (a label or period, then a value); a **KPI** one value; a **Table** any columns.

## Visual types

- **KPI**: one number, optionally with its change against a comparison period.
- **Line** and **Bars**: one series over a period or category.
- **Table**: rows with local sorting.
- **Text / note**: a title and notes (one line: `|` starts a new line, `- ` a bullet).
- **Image**: the theme's logo or an image you insert (Theme menu).

Numbers follow the report's language (`1.234,56` in pt-BR, `1,234.56` in en-US). An empty value is shown as empty, never as zero.

## Period

**Period** on the toolbar sets the report's time window: **Last 12 months**, **Year to date**, **Last 30 days**, **Previous year**, **All time**. Metric cards use it directly; SQL cards read it through the `:period_start` and `:period_end` parameters.

## Report filters

A filter is a report-wide choice (region, channel, status...) shown in a bar above the page. **Add a filter** asks for its name, label, type and connection, and optionally a read-only query that fills its dropdown.

Cards use a filter as a parameter in their SQL. Lume puts the chosen value in as a quoted literal, or `NULL` for "All", so a card is written like this:

```sql
SELECT region, SUM(total)
FROM orders
WHERE (:region IS NULL OR region = :region)
GROUP BY region
```

A card lists the filters it uses under **Filters:**. A filter a card cannot honour blocks that card with the reason, rather than showing unfiltered numbers. Filter values are never logged or kept in history.
