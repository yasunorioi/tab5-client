---
name: tab5-client-repo
description: 均平 rover 案件のリポジトリの場所と、private のまま upstream 追跡している構成
metadata:
  type: reference
---

- ローカル: `C:\Users\kita_\dev\tab5-client`
- リモート: `origin` = https://github.com/yasunorioi/tab5-client （**private**）
  / `upstream` = https://github.com/yasunorioi/tab5-caster （public）
- デフォルトブランチは `master`。

GitHub の fork ボタンは使っていない（**public repo の fork は private にできない**ため、
clone を新しい private リポジトリに push する形にした）。upstream 追跡はこれで残る。

設計と実機実測は `docs/hardware-findings.md` / `docs/design.md` / `docs/todo.md` にあり、
このメモリのコピーが `notes/memory/` に入っている。案件の中身は [[tab5-leveling-rover]]。

**Why:** 次回セッションでどこを開けば文脈が全部あるかが一発で分かるように。
**How to apply:** 作業再開時はまず `docs/todo.md` を読む。
