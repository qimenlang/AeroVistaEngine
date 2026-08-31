# 脚本索引

本目录脚本与 `scripts/` 实际文件保持一致；新增脚本须在此登记（见 `doc-sync.mdc`）。

| 脚本 | 用途 | 何时运行 |
| --- | --- | --- |
| `check_encoding.py` | 源码编码检查（UTF-8 + mojibake 两层） | pre-commit / CI |
| `clang_tidy_engine.py` | clang-tidy 引擎检查（命名 + 认知复杂度），需 `compile_commands.json` | pre-commit / CI |
| `lizard_engine.py` | lizard 圈复杂度（CCN）检查，阈值 15 | pre-commit / CI |
| `ci_changed_complexity.py` | CI 辅助：对 vs git base 变更的文件跑 clang-tidy + lizard | CI |
| `doc_sync_check.py` | 文档同步核查（列设计文档 / 检索残留 / 校验索引） | `/doc-sync` skill 调用 |
