---
title: Themes and export
---

# Themes and export

## Themes

**Theme** on the report toolbar chooses the look:

- **Light**, **Dark** or **Executive**, built in;
- **Workspace theme (Brand Kit)**: one theme shared by every report of the [workspace](../workspace/workspaces.md). New reports start with it when it exists, and changing it updates every report that uses it;
- **Open theme file...**: a theme file (`.lrtheme`) anywhere, referenced by a path relative to the report, so a team can share one look.

A theme holds the colours, the series palette, type sizes, corners, the card style (flat, shadow, border, accent bar on top), the KPI style (plain, accent bar, tinted) and an optional header band with a logo and title. Edit them in the page's properties panel or under **More theme options...**.

From the Theme menu:

- **Save theme as file...** and **Use as the workspace theme (Brand Kit)**;
- **Choose the theme's logo...** (PNG or JPEG), **Insert the logo as a card**, **Insert image...**;
- **Detach (keep it in the report only)**.

Good to know:

- A report may override parts of its theme; only the differences are kept. A theme marked **locked** ignores overrides, so the brand stays the brand.
- Every change is checked for text contrast (4.5:1). A combination that fails is refused with the ratio.
- When a theme file goes missing, the report uses the copy it saved and says so.
- **Save As...** of a report whose theme Lume created moves the theme and its logo along with it.

## Export

**Export** on the report toolbar:

| Choice | Gives |
|---|---|
| **Report as an HTML page (copyable text, sharp charts)...** | one self-contained file with every page, the theme and the logo embedded; charts as vector graphics with values on hover and a copyable values table; no scripts, nothing loaded from outside. Your browser's Print > Save as PDF turns it into a vector PDF |
| **Page as PDF (image)...** | the current page as a PDF holding an image |
| **Page as PNG image...** / **Page as high-resolution PNG...** | the current page |
| **Selected card as PNG...** | one card |
| **Selected card data (CSV)...** | the selected card's rows |
| **Page data to Excel (XLSX, one sheet per card)...** | the page's data |

An export waits until every card has finished querying; otherwise it writes nothing and says so.
