---
title: Source Control (Git)
---

# Source Control (Git)

The **Source Control** panel (**Tools > Source Control**, or **Show Source Control** in the command palette) works with Git repositories on your computer: status, staging by hunk, commit, branches, pull, push, stashes and history. It uses the `git` installed on your computer.

**Git versions files; it never touches a database.** Committing a `.sql` file, even one with `DROP TABLE` in it, leaves every database exactly as it was.

## Open a repository

- **Open Another Repository...**: a folder that is (or is inside) a repository.
- **Init...**: create a repository in a folder.
- **Clone**: a URL and an empty or new folder. A failed or cancelled clone removes the folder it created.

The panel shows the branch, how far it is ahead / behind its upstream, and the identity commits will use (from your Git configuration, which Lume never changes).

## Changes and commit

- **Changes** lists staged and unstaged files. **Stage** / **Unstage**, **Stage All** / **Unstage All**, **Discard Changes...**, **Delete File...** (untracked). Filter the list by name; **Include untracked files** shows new files.
- Click a file to open its **diff** in a tab: **Side by side** or **Inline**, against the index, staged, or against HEAD. **Stage hunk** / **Unstage hunk** stage part of a file.
- Write a **Commit message (summary)** and an optional **Description**, then **Commit** or **Commit & Push**. Commit takes **only staged changes** and is disabled, with the reason, when nothing is staged. Your Git hooks run. If the commit fails, the message is kept.

To edit a file, open it from the panel or with **Open File...**: it opens in a file tab, saved with **Cmd+S**.

## Sync

- **Pull** follows the strategy you chose: **Fast-forward only** (default), **Merge** or **Rebase** (repository menu, or Settings > Source Control). Lume never stashes your changes on its own.
- **Push** never forces. Without an upstream it sets one (**Push (sets upstream)**). A rejected push says why and what to do next.
- **Fetch** updates the remote branches.

Lume never asks for or stores Git passwords: configure a credential helper or your SSH agent. Remote URLs that contain credentials are masked everywhere.

Before **Pull**, **Switch branch**, **Merge**, **Stash** and **Discard**, Lume checks for unsaved file tabs inside the repository and offers **Save all and continue** or **Cancel**.

## Branches, merges and stashes

- **Branches**: **Switch**, **New branch** (**Create & Switch**), **Merge**, **Delete branch**, and **Force Delete (loses unmerged commits)...** as a separate item.
- **Conflicts**: resolve them in the file, mark the file **Resolved**, then **Commit Merge** (or **Continue rebase**). **Abort Merge** / **Abort Rebase** go back.
- **Stashes**: **Stash** (optionally with untracked files and a message), **Apply**, **Pop**, **Drop**.
- **History**: the commits, each with its changes; **Copy Hash**, **Copy Patch**. **File History** shows the log of one file.
- **Remotes**: **Add remote** and **Remove remote**.

Destructive actions (discard, delete, drop, force delete, abort) are confirmed with the file and the scope and say there is no undo. They are written to a local audit log.

The panel watches the repository while it is visible and refreshes when files change. Files open in tabs reload after a checkout or pull; a tab with unsaved edits is never overwritten and offers **Compare**, **Keep mine** or **Reload**.

## Lume Intelligence

With [Lume Intelligence](../intelligence/overview.md) set up:

- **Suggest (AI)** proposes a commit message from the **staged diff only**.
- **Review (AI)** lists risks in the staged change: dropped or renamed tables and columns, data loss, `UPDATE` or `DELETE` without `WHERE`, migrations that cannot be reversed, long locks, new foreign keys without an index.

Both first show exactly what would be sent. `.env` files, keys, certificates, credential files and Lume's own connection files are left out (and counted). **Send** is a second, explicit click. The answer fills the message fields or appears under the commit box; nothing is ever committed for you.

## Not available yet

A three-way merge editor, renaming branches, creating a branch at a chosen commit, comparing a file with a chosen version, force push, pull requests and other hosting features, and rebinding the Git shortcuts.

If Git is not found, set its path with a `git=/path/to/git` line in `git.txt` in Lume's preferences folder (shown in Settings > General).
