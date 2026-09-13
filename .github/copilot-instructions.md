# Copilot 指引 / Copilot instructions

本仓库给 AI 编码代理的完整说明在根目录 [AGENTS.md](../AGENTS.md)，开始任何工作前请先完整阅读它。

核心原则 / Core principle：**尽量少编译，能用现成的直接用**——理解 / 评审 / 移植靠静态阅读（结论用「文件 + 行号」引用）；需要能跑的程序直接用 [Releases](../../releases) 现成构建；third_party 原样使用；确需编译时一次过，不循环试错。详见 AGENTS.md。

The full AI-agent guide lives in the root [AGENTS.md](../AGENTS.md) — read it in full before doing anything. Core principle: **build rarely, reuse what already works** — understand / review / port by static reading (cite file + line); grab runnable binaries from Releases instead of building; use vendored third_party as-is; if a build is truly needed, get it right in one pass — no trial-and-error loops.
