#!/usr/bin/env python3
"""
Generate CODE_METRICS_REPORT.md: LoC, comment metrics, branching proxy (complexity),
readability index on comments, qualitative scores. No third-party dependencies.
"""
from __future__ import annotations

import re
import sys
from dataclasses import dataclass
from pathlib import Path


# Project root = directory containing this script (do not use parents[1] — that escapes the repo).
ROOT = Path(__file__).resolve().parent
REPORT_NAME = "CODE_METRICS_REPORT.md"
SOURCE_GLOBS = ("**/*.cpp", "**/*.hpp", "**/*.h")
# Skip these path segments when scanning (nested clones, caches, build artifacts).
SKIP_DIR_PARTS = frozenset(
    {".git", "__pycache__", ".venv", "venv", "node_modules", "build", "dist", ".vs"}
)


@dataclass
class FileMetrics:
    rel_path: str
    physical_lines: int
    blank_lines: int
    code_lines: int
    comment_lines: int
    comment_ratio: float
    decision_points: int
    cc_estimate: int
    avg_line_len: float
    doxygen_hits: int
    ari_comments: float | None
    readability_score: float
    comment_quality_score: float
    readability_label: str
    comment_quality_label: str


def strip_cpp_comments(text: str) -> tuple[str, int, int]:
    """Remove /* */ and // comments; return (code-ish text, code line count, comment line count)."""
    without_block = re.sub(r"/\*[\s\S]*?\*/", lambda m: "\n" * m.group(0).count("\n"), text)
    code_lines = 0
    comment_lines = 0
    out_lines: list[str] = []
    for line in without_block.splitlines():
        if "//" in line:
            code_part = line.split("//", 1)[0]
            if code_part.strip():
                code_lines += 1
                out_lines.append(code_part)
            else:
                comment_lines += 1
                out_lines.append("")
        else:
            if line.strip():
                code_lines += 1
            out_lines.append(line)
    stripped = "\n".join(out_lines)
    return stripped, code_lines, comment_lines


def strip_py_comments(text: str) -> tuple[str, int, int]:
    lines = text.splitlines()
    code_n = 0
    comm_n = 0
    stripped_lines: list[str] = []
    for ln in lines:
        s = ln.strip()
        if not s:
            stripped_lines.append("")
            continue
        if s.startswith("#"):
            comm_n += 1
            stripped_lines.append("")
            continue
        if "#" in ln:
            q = ln.split("#")[0]
            if q.strip():
                code_n += 1
                stripped_lines.append(q)
            else:
                comm_n += 1
                stripped_lines.append("")
        else:
            code_n += 1
            stripped_lines.append(ln)
    return "\n".join(stripped_lines), code_n, comm_n


def extract_comment_text(path: Path, text: str) -> str:
    """Rough extraction of comment bodies for readability stats."""
    parts: list[str] = []
    if path.suffix in {".cpp", ".hpp", ".h"}:
        for m in re.finditer(r"/\*([\s\S]*?)\*/", text):
            parts.append(m.group(1))
        for m in re.finditer(r"//(.+)$", text, re.MULTILINE):
            parts.append(m.group(1))
    elif path.suffix == ".py":
        for m in re.finditer(r"^\s*#+\s*(.+)$", text, re.MULTILINE):
            parts.append(m.group(1))
        for m in re.finditer(r'"""([\s\S]*?)"""', text):
            parts.append(m.group(1))
        for m in re.finditer(r"'''([\s\S]*?)'''", text):
            parts.append(m.group(1))
    return " ".join(parts)


def automated_readability_index(text: str) -> float | None:
    t = re.sub(r"\s+", " ", text).strip()
    if len(t) < 80:
        return None
    chars = len(re.sub(r"\s", "", t))
    words = len(t.split())
    if words < 10:
        return None
    sents = max(1, len(re.split(r"[.!?]+", t)) - 1)
    if sents < 1:
        sents = 1
    return 4.71 * (chars / words) + 0.5 * (words / sents) - 21.43


def count_decisions(code_without_strings: str) -> int:
    """Proxy for cyclomatic pressure: branching keywords and boolean operators."""
    s = code_without_strings
    n = 0
    patterns = [
        r"\bif\s*\(",
        r"\belse\s+if\s*\(",
        r"\bfor\s*\(",
        r"\bwhile\s*\(",
        r"\bdo\s*\{",
        r"\bswitch\s*\(",
        r"\bcase\b",
        r"\bcatch\s*\(",
        r"\?\s*[^;{}]+\s*:",  # ternary
    ]
    for p in patterns:
        n += len(re.findall(p, s))
    n += s.count("&&")
    n += s.count("||")
    return n


def mask_strings_cpp(s: str) -> str:
    out: list[str] = []
    i = 0
    while i < len(s):
        if s[i] in "\"'":
            q = s[i]
            out.append(" ")
            i += 1
            while i < len(s):
                if s[i] == "\\" and i + 1 < len(s):
                    i += 2
                    continue
                if s[i] == q:
                    i += 1
                    break
                i += 1
            continue
        out.append(s[i])
        i += 1
    return "".join(out)


def count_doxygen(text: str) -> int:
    keys = (
        r"@brief",
        r"@param",
        r"@return",
        r"@details",
        r"@pre",
        r"@post",
        r"@note",
        r"@warning",
    )
    return sum(len(re.findall(k, text)) for k in keys)


def score_readability(
    avg_len: float,
    physical: int,
    comment_ratio: float,
    is_header: bool,
) -> tuple[float, str]:
    """Heuristic 0–10: line length, balance, file role."""
    score = 7.0
    if avg_len > 120:
        score -= 2.0
    elif avg_len > 100:
        score -= 1.0
    elif avg_len < 55 and physical > 200:
        score -= 0.3
    if physical > 800:
        score -= 0.8
    if physical > 2000:
        score -= 0.7
    if is_header:
        if comment_ratio < 0.15:
            score -= 0.5
    else:
        if comment_ratio < 0.03 and physical > 400:
            score -= 0.8
        if comment_ratio > 0.45:
            score -= 0.5
    score = max(1.0, min(10.0, score))
    if score >= 8:
        lab = "High"
    elif score >= 6:
        lab = "Moderate"
    elif score >= 4:
        lab = "Fair"
    else:
        lab = "Low"
    return score, lab


def score_comment_quality(
    doxygen_hits: int,
    comment_ratio: float,
    physical: int,
    ari: float | None,
) -> tuple[float, str]:
    score = 6.0
    if doxygen_hits > 20:
        score += 1.5
    elif doxygen_hits > 8:
        score += 0.8
    elif doxygen_hits > 0:
        score += 0.3
    if 0.08 <= comment_ratio <= 0.35:
        score += 0.7
    elif comment_ratio < 0.02 and physical > 300:
        score -= 1.2
    if ari is not None:
        if 8 <= ari <= 14:
            score += 0.5
        elif ari > 18:
            score -= 0.4
    score = max(1.0, min(10.0, score))
    if score >= 8:
        lab = "Strong"
    elif score >= 6:
        lab = "Adequate"
    elif score >= 4:
        lab = "Thin"
    else:
        lab = "Weak"
    return score, lab


def analyze_file(path: Path) -> FileMetrics:
    text = path.read_text(encoding="utf-8", errors="replace")
    physical_lines = text.count("\n") + (1 if text and not text.endswith("\n") else 0)
    blank_lines = sum(1 for ln in text.splitlines() if not ln.strip())

    if path.suffix == ".py":
        _, code_lines, comment_lines = strip_py_comments(text)
        stripped_for_decisions = mask_strings_cpp(
            strip_py_comments(text)[0]
        )  # strings rare in py this way
    else:
        stripped, code_lines, comment_lines = strip_cpp_comments(text)
        stripped_for_decisions = mask_strings_cpp(stripped)

    comment_ratio = (
        comment_lines / physical_lines if physical_lines else 0.0
    )
    decision_points = count_decisions(stripped_for_decisions)
    cc_estimate = 1 + decision_points

    non_blank = [ln for ln in text.splitlines() if ln.strip()]
    avg_line_len = (
        sum(len(ln) for ln in non_blank) / len(non_blank) if non_blank else 0.0
    )

    ctext = extract_comment_text(path, text)
    ari = automated_readability_index(ctext)
    dox = count_doxygen(text)
    is_header = path.suffix in {".hpp", ".h"}
    rs, rl = score_readability(avg_line_len, physical_lines, comment_ratio, is_header)
    cq, cl = score_comment_quality(dox, comment_ratio, physical_lines, ari)

    return FileMetrics(
        rel_path=str(path.relative_to(ROOT)).replace("\\", "/"),
        physical_lines=physical_lines,
        blank_lines=blank_lines,
        code_lines=code_lines,
        comment_lines=comment_lines,
        comment_ratio=round(comment_ratio * 100, 1),
        decision_points=decision_points,
        cc_estimate=cc_estimate,
        avg_line_len=round(avg_line_len, 1),
        doxygen_hits=dox,
        ari_comments=round(ari, 2) if ari is not None else None,
        readability_score=round(rs, 1),
        comment_quality_score=round(cq, 1),
        readability_label=rl,
        comment_quality_label=cl,
    )


def _skipped_path(path: Path) -> bool:
    rel = path.relative_to(ROOT)
    return any(part in SKIP_DIR_PARTS for part in rel.parts)


def collect_paths() -> list[Path]:
    """All C/C++ headers and sources plus Python under ROOT only (no parent/sibling trees)."""
    paths: list[Path] = []
    for pattern in SOURCE_GLOBS:
        for p in ROOT.glob(pattern):
            if p.is_file() and not _skipped_path(p):
                paths.append(p.resolve())
    for p in ROOT.rglob("*.py"):
        if p.is_file() and not _skipped_path(p):
            paths.append(p.resolve())
    return sorted(set(paths), key=lambda p: str(p).lower())


def main() -> int:
    files = collect_paths()
    if not files:
        print("No source files found.", file=sys.stderr)
        return 1

    rows: list[FileMetrics] = [analyze_file(p) for p in files]
    total_phys = sum(r.physical_lines for r in rows)
    total_code = sum(r.code_lines for r in rows)

    lines_out: list[str] = []
    lines_out.append("# Code metrics report")
    lines_out.append("")
    lines_out.append(f"**Workspace:** `{ROOT.name}` (root: `{ROOT}`)  ")
    lines_out.append("")
    lines_out.append("This report was generated by `generate_code_metrics_report.py` in the same directory. ")
    lines_out.append("Metrics are **approximate** and intended for trend review, not certification.")
    lines_out.append("")
    lines_out.append("## Methodology")
    lines_out.append("")
    lines_out.append("| Metric | Meaning |")
    lines_out.append("|--------|---------|")
    lines_out.append(
        "| **Physical LoC** | Total newline-terminated lines in the file |"
    )
    lines_out.append(
        "| **Stripped code ~** | Non-blank lines after removing `/* */` and `//` (declarations only in heavy Doxygen headers — **small here is normal**; use physical LoC for total size) |"
    )
    lines_out.append(
        "| **Line-comment ~** | Lines that are only `//` or `#` (end-of-line); block `/* */` is removed before counting, not added here |"
    )
    lines_out.append(
        "| **Cmt % (line)** | Line-comment ~ ÷ physical lines (underestimates documentation in big block comments) |"
    )
    lines_out.append(
        "| **Decision points** | Count of `if`/`for`/`while`/`case`/`catch`/`else if`/ternary/`&&`/`||` in code (strings masked) |"
    )
    lines_out.append(
        "| **CC estimate (file)** | `1 + decision points` — **not** ISO McCabe per function; use as relative load only |"
    )
    lines_out.append(
        "| **ARI (comments)** | [Automated Readability Index](https://en.wikipedia.org/wiki/Automated_readability_index) on extracted comment text; lower ≈ easier English; `n/a` if too little text |"
    )
    lines_out.append(
        "| **Readability (score)** | Heuristic 1–10 from average line length, file size, comment balance |"
    )
    lines_out.append(
        "| **Comment quality (score)** | Heuristic 1–10 from Doxygen tag density, comment %, ARI |"
    )
    lines_out.append("")
    lines_out.append(
        f"**Totals:** {len(rows)} files, **{total_phys:,}** physical lines, **~{total_code:,}** stripped code lines (approx.)."
    )
    lines_out.append("")
    lines_out.append("## Per-file summary")
    lines_out.append("")
    lines_out.append(
        "| File | Phys LoC | Stripped code ~ | Line-cmt ~ | Cmt % (line) | Decision pts | CC est. | Avg len | Doxygen tags | ARI (cmt) | Readability | Comment qual. |"
    )
    lines_out.append(
        "|------|---------:|-----------------:|-----------:|-------------:|-------------:|--------:|--------:|-------------:|----------:|:------------|:--------------|"
    )
    for r in rows:
        ari_s = str(r.ari_comments) if r.ari_comments is not None else "n/a"
        lines_out.append(
            f"| `{r.rel_path}` | {r.physical_lines} | {r.code_lines} | {r.comment_lines} | {r.comment_ratio}% | "
            f"{r.decision_points} | {r.cc_estimate} | {r.avg_line_len} | {r.doxygen_hits} | {ari_s} | "
            f"{r.readability_score} ({r.readability_label}) | {r.comment_quality_score} ({r.comment_quality_label}) |"
        )
    lines_out.append("")
    lines_out.append("## Narrative summary by file")
    lines_out.append("")
    for r in rows:
        lines_out.append(f"### `{r.rel_path}`")
        lines_out.append("")
        lines_out.append(
            f"- **Size:** {r.physical_lines} physical lines; ~{r.code_lines} **stripped** code lines "
            f"(block comments removed — headers with large `/** */` Doxygen show small stripped counts; physical LoC is the workload indicator)."
        )
        lines_out.append(
            f"- **Line-only comments:** ~{r.comment_lines} lines ({r.comment_ratio}% of physical); "
            f"Doxygen-style tags (**@brief**, **@param**, …) ≈ **{r.doxygen_hits}** occurrences in raw file."
        )
        lines_out.append(
            f"- **Branching proxy:** {r.decision_points} decision points → **CC estimate (file) = {r.cc_estimate}** "
            f"(compare across files only; for refactor targets, use a per-function tool such as `lizard`)."
        )
        if r.ari_comments is not None:
            lines_out.append(
                f"- **Comment readability (ARI):** {r.ari_comments} (typical U.S. grade level ≈ ARI + 5–6; rough guide only)."
            )
        else:
            lines_out.append("- **Comment readability (ARI):** not computed (insufficient comment prose).")
        lines_out.append(
            f"- **Readability (heuristic):** **{r.readability_score}/10** — *{r.readability_label}* "
            f"(line-length and size weighted)."
        )
        lines_out.append(
            f"- **Comment quality (heuristic):** **{r.comment_quality_score}/10** — *{r.comment_quality_label}* "
            f"(structure/tags vs. density)."
        )
        lines_out.append("")

    out_path = ROOT / REPORT_NAME
    out_path.write_text("\n".join(lines_out), encoding="utf-8")
    print(f"Wrote {out_path}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
