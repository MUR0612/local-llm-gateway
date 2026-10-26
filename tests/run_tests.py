#!/usr/bin/env python3
"""Runs the offline integration suite (Linux only, standard library only)."""
import argparse
import os
import sys
import unittest

sys.dont_write_bytecode = True  # keep __pycache__ (with local paths) out of the tree
HERE = os.path.dirname(os.path.abspath(__file__))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bin-dir", default=os.path.dirname(HERE))
    parser.add_argument("--fault-lib")
    parser.add_argument("--sanitizer", choices=["", "asan", "tsan"], default="")
    parser.add_argument("--valgrind", action="store_true",
                        help="run only the Valgrind memcheck suite")
    parser.add_argument("-k", action="append", dest="patterns",
                        help="only run tests matching this substring")
    parser.add_argument("-q", action="store_true", help="less output")
    args = parser.parse_args()

    os.environ["LLMGW_TEST_BIN_DIR"] = os.path.abspath(args.bin_dir)
    os.environ["LLMGW_TEST_SANITIZER"] = args.sanitizer
    if args.fault_lib:
        os.environ["LLMGW_TEST_FAULT_LIB"] = os.path.abspath(args.fault_lib)
    for name in ("LLMGW_STATS_PORT", "LLMGW_BACKEND_CMD", "LLMGW_BACKEND_PORT",
                 "LLMGW_HOME_PAGE", "LLMGW_DEBUG"):
        os.environ.pop(name, None)

    sys.path.insert(0, os.path.join(HERE, "integration"))
    loader = unittest.TestLoader()
    if args.patterns:
        loader.testNamePatterns = ["*%s*" % p for p in args.patterns]
    if args.valgrind:
        suite = loader.loadTestsFromName("test_valgrind")
    else:
        suite = unittest.TestSuite()
        for name in sorted(os.listdir(os.path.join(HERE, "integration"))):
            if name.startswith("test_") and name.endswith(".py") \
                    and name != "test_valgrind.py":
                suite.addTests(loader.loadTestsFromName(name[:-3]))
    result = unittest.TextTestRunner(verbosity=1 if args.q else 2).run(suite)
    return 0 if result.wasSuccessful() else 1


if __name__ == "__main__":
    sys.exit(main())
