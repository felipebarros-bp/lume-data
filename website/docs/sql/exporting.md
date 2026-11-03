---
title: Exporting results
---

# Exporting results

**Export...** on the results toolbar, **Query > Results > Export Results...** or **Shift+Cmd+E** opens the export dialog, then your system's save panel.

## Formats

CSV, TSV, JSON, Markdown, SQL INSERT and XLSX. The options shown depend on the format:

| Option | Choices |
|---|---|
| **Rows** | **Selection**, **Loaded rows**, or **Full result** (see below). A note under it says what will be written, with the count |
| **Values** | **Raw values** (as the database sent them) or **Displayed values** (with the Format menu applied) |
| **Header line** | include the column names |
| **Guard formulas** | puts `'` before text starting with `= + - @`, so a spreadsheet never runs it as a formula. On by default; numbers in numeric columns are left alone |
| **NULL as** | empty, `NULL` or `\N` |
| **Delimiter** (CSV) | Comma, Semicolon, Tab, Pipe |
| **Quotes** | As needed, Always |
| **Line ending** | LF, CRLF |
| **Table name** (SQL INSERT) | the table the INSERTs target |
| **Stop after** (full result) | a number of rows, megabytes or seconds |

The button says what it will do ("Export 14 rows..." or "Export full result...").

- **XLSX** writes text as text (a cell can never become a formula) and numbers and booleans as typed values. It stops at the sheet limits (1,048,576 rows, 16,384 columns) and says so.
- **SQL INSERT** quotes values so re-running the file gives the same rows; binary values are written in each database's binary literal form.
- Values the driver did not load in full (large objects) are written as a placeholder in CSV / JSON and as `NULL` with a comment in SQL INSERT, never as fake bytes.

## Full result

**Full result** writes the loaded rows, then keeps reading from the open cursor in blocks, writing and dropping them, so memory stays flat even for millions of rows. Progress shows in the window title; **Esc** cancels and removes the partial file. A row, size or time limit stops the export and keeps the file. When the cursor is already closed, the dialog says the query must run again and that the result may differ.

Files are written under a temporary name and renamed at the end, so a cancelled or failed export never leaves a half-written file with the final name.

Exporting is refused while the tab has unsaved edits.

## Custom extractors (advanced)

An extractor is a program of yours that receives the rows and writes its own format. Register it in `extractors.txt` in Lume's preferences folder (shown in Settings > General), one line per extractor:

```
name<TAB>/absolute/path/to/program --option<TAB>timeout=60
```

It then appears under **Extractor** in the export dialog. Each line must be approved there once (an edited line needs approval again). Extractors run without a shell, inside the operating system's sandbox (macOS sandbox, Linux bubblewrap), with no network, limited time, CPU, memory and output, and may write only to their own scratch folder. Every run is logged. Where no sandbox is available, extractors are disabled.

## Run straight to a file

**Query > Run Special > To File...** runs the statement and writes its first result to the chosen file. See [Running SQL](running.md#execute-to-file).
