#!/usr/bin/env python3
"""
remove_qcad_trial.py

Removes QCAD trial version watermarks and random lines from SVG files.
Handles output from QCAD 3.x (uses <line> elements) and older versions
(uses <path> elements wrapped in <g> blocks).

Usage:
    python remove_qcad_trial.py input.svg
    python remove_qcad_trial.py input.svg output.svg
"""

import re
import sys
from pathlib import Path


def remove_trial(content: str) -> tuple[str, int, int]:
    """
    Remove QCAD trial watermark blocks and their associated random lines.

    Returns:
        (cleaned_content, watermark_blocks_removed, random_lines_removed)
    """
    before = content.count("Trial Version")

    # --- Format A: QCAD 3.x ---
    # Comment block + 3x (<!-- Line --> + <line .../>) with no wrapping <g>
    format_a = re.compile(
        r"\s*<!-- Text: QCAD\.org\r?\nTrial Version\r?\nRandom lines added -->"
        r"(?:\s*<!-- Line -->\s*<line[^/]*/>\s*){3}",
        re.DOTALL,
    )
    result, n_a = format_a.subn("", content)

    # --- Format B: older QCAD ---
    # Comment block + <g>...</g> (path-based watermark text), followed by 3
    # random <path> lines, then possibly 3 more trailing lines before </svg>.
    format_b = re.compile(
        r"\s*<!-- Text: QCAD\.org\s*Trial Version\s*Random lines added -->"
        r".*?</g>"
        r"(?:\s*<!-- Line -->\s*<path[^/]*/>\s*){3}",
        re.DOTALL,
    )
    result, n_b = format_b.subn("", result)

    # Format B trailing lines (last block's 3 lines before </g></svg>)
    result, n_tail = re.subn(
        r"(\s*<!-- Line -->\s*<path[^/]*/>\s*){3}(\s*</g>\s*</svg>)",
        r"\2",
        result,
        count=1,
    )

    after = result.count("Trial Version")
    blocks_removed = before - after
    lines_removed = n_a * 3 + n_b * 3 + (3 if n_tail else 0)
    return result, blocks_removed, lines_removed


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        sys.exit(1)

    input_path = Path(sys.argv[1])
    if not input_path.exists():
        print(f"Error: file not found: {input_path}")
        sys.exit(1)

    output_path = (
        Path(sys.argv[2]) if len(sys.argv) >= 3
        else input_path.with_stem(input_path.stem + "_cleaned")
    )

    content = input_path.read_text(encoding="utf-8")
    cleaned, blocks_removed, lines_removed = remove_trial(content)

    if blocks_removed == 0 and lines_removed == 0:
        print("No QCAD trial watermarks found — file may use an unrecognised format.")
        print("Try opening the file in a text editor and searching for 'Trial Version'")
        print("to see what the surrounding structure looks like.")
    else:
        output_path.write_text(cleaned, encoding="utf-8")
        print(f"Removed {blocks_removed} watermark block(s) and {lines_removed} random line(s).")
        print(f"Saved to: {output_path}")


if __name__ == "__main__":
    main()
