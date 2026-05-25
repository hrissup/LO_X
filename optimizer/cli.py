
from __future__ import annotations

import argparse
import os
import sys

from . import __version__
from .optimizer import analyze_file_cpp, optimize_file


def _colour(text: str, code: str) -> str:
    if sys.stdout.isatty():
        return f"\033[{code}m{text}\033[0m"
    return text


def _green(t):  return _colour(t, "32")
def _yellow(t): return _colour(t, "33")
def _red(t):    return _colour(t, "31")
def _bold(t):   return _colour(t, "1")



def cmd_analyze(args: argparse.Namespace) -> int:
    """Analyse *args.source* and print a report."""
    try:
        from .analyzer import analyse_file
        from .dependence import check_dependence
    except ImportError:
        try:
            output = analyze_file_cpp(args.source, func=getattr(args, "func", None))
            print(output, end="")
            return 0
        except RuntimeError as exc:
            print(_red(f"Analyzer error: {exc}"), file=sys.stderr)
            return 1
    src = args.source
    if not os.path.isfile(src):
        print(_red(f"Error: file not found: {src}"), file=sys.stderr)
        return 1

    print(_bold(f"\nLocusOpt P2 v{__version__} — Analysis Report"))
    print("=" * 56)
    print(f"File: {src}\n")

    try:
        reports = analyse_file(src, func_name=getattr(args, "func", None))
    except (SyntaxError, FileNotFoundError) as exc:
        print(_red(f"Parse error: {exc}"), file=sys.stderr)
        return 1

    if not reports:
        print(_yellow("No analysable kernel functions found."))
        return 0

    for report in reports:
        print(report.summary())

        for i, nest in enumerate(report.kernel_info.loop_nests, 1):
            dep = check_dependence(nest)
            print(f"\n  Dependence check (nest #{i}):")
            print(f"    Safe interchange : {_green('yes') if dep.safe_interchange else _red('no')}")
            print(f"    Safe tiling      : {_green('yes') if dep.safe_tiling else _red('no')}")
            print(f"    {dep.reason}")
            for d in dep.details:
                print(f"      · {d}")

    return 0


def cmd_optimize(args: argparse.Namespace) -> int:
    """Optimize *args.source* and emit the transformed C source."""
    src = args.source
    if not os.path.isfile(src):
        print(_red(f"Error: file not found: {src}"), file=sys.stderr)
        return 1

    try:
        optimized = optimize_file(
            src,
            output=args.output,
            func=getattr(args, "func", None),
            tile=args.tile,
            disable_interchange=args.no_interchange,
            disable_tiling=args.no_tiling,
        )
    except RuntimeError as exc:
        print(_red(f"Optimizer error: {exc}"), file=sys.stderr)
        return 1

    if optimized and not args.output:
        print(optimized, end="")

    return 0


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="locusopt-p2",
        description="LocusOpt P2 — cache-locality analyser for C numerical kernels (Phase 2)",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=(
            "Examples:\n"
            "  python main.py analyze kernels/matrix_transpose.c\n"
            "  python main.py analyze kernels/matrix_transpose.c --func kernel_transpose\n"
            "  python main.py optimize kernels/matrix_transpose.c --output /tmp/opt.c\n"
            "  # Sample kernel lives in kernels/matrix_transpose.c\n"
        ),
    )
    parser.add_argument(
        "--version", action="version", version=f"%(prog)s {__version__}"
    )

    sub = parser.add_subparsers(dest="command", required=True)

    p_an = sub.add_parser("analyze", help="Analyse cache locality of a kernel source file")
    p_an.add_argument("source", help="C source file to analyse")
    p_an.add_argument("--func", metavar="NAME",
                      help="Specific kernel function to analyse")

    p_opt = sub.add_parser("optimize", help="Emit cache-friendly optimized source")
    p_opt.add_argument("source", help="C source file to optimize")
    p_opt.add_argument("--output", "-o", help="Write optimized source to file")
    p_opt.add_argument("--func", metavar="NAME",
                       help="Specific kernel function to optimize")
    p_opt.add_argument("--tile", type=int, default=32,
                       help="Tile size for loop blocking (default: 32)")
    p_opt.add_argument("--no-interchange", action="store_true",
                       help="Disable loop interchange")
    p_opt.add_argument("--no-tiling", action="store_true",
                       help="Disable loop tiling")

    return parser


def main(argv=None) -> int:
    parser = build_parser()
    args = parser.parse_args(argv)
    if args.command == "analyze":
        return cmd_analyze(args)
    if args.command == "optimize":
        return cmd_optimize(args)
    parser.print_help()
    return 1
