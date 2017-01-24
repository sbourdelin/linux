#!/usr/bin/env python3
# -*- coding: utf-8; mode: python -*-
# pylint: disable=C0103

u"""
    src2rst
    ~~~~~~~

    Implementation of the ``kerneldoc-src2rst`` command.

    :copyright:  Copyright (C) 2016  Markus Heiser
    :license:    GPL Version 2, June 1991 see Linux/COPYING for details.

    The ``kerneldoc-src2rst`` command extracts documentation from Linux kernel's
    source code comments, see ``--help``::

        $ kerneldoc-src2rst --help

"""

# ------------------------------------------------------------------------------
# imports
# ------------------------------------------------------------------------------

import sys
import argparse
import re
import multiprocessing

import six

from fspath import FSPath, OS_ENV
import kernel_doc as kerneldoc

# ------------------------------------------------------------------------------
# config
# ------------------------------------------------------------------------------

MARKUP = "kernel-doc" # "reST"
MSG    = lambda msg: sys.__stderr__.write("INFO : %s\n" % msg)
ERR    = lambda msg: sys.__stderr__.write("ERROR: %s\n" % msg)
FATAL  = lambda msg: sys.__stderr__.write("FATAL: %s\n" % msg)
IGNORE = ['kernel-doc.rst']

TEMPLATE_INDEX=u"""\
.. -*- coding: utf-8; mode: rst -*-

================================================================================
%(title)s
================================================================================

.. toctree::
    :maxdepth: 1

"""

CMD     = None # global used by multiprocessing
SRCTREE = FSPath(OS_ENV.get("srctree", ""))

# ------------------------------------------------------------------------------
def main():
# ------------------------------------------------------------------------------

    global CMD # pylint: disable=W0603, W0621

    CLI = argparse.ArgumentParser(
        description = ("Parse *kernel-doc* comments from source code")
        , formatter_class=argparse.ArgumentDefaultsHelpFormatter)

    CLI.add_argument(
        "srctree"
        , help    = "Folder of source code."
        , type    = lambda x: FSPath(x).ABSPATH)

    CLI.add_argument(
        "doctree"
        , help    = "Folder to place reST documentation."
        , type    = lambda x: FSPath(x).ABSPATH)

    CLI.add_argument(
        "--sloppy"
        , action  = "store_true"
        , help    = "Sloppy comment check, reports only severe errors.")

    CLI.add_argument(
        "--force"
        , action  = "store_true"
        , help    = "Don't stop if doctree exists.")

    CLI.add_argument(
        "--threads"
        , type    =  int
        , default = multiprocessing.cpu_count()
        , help    = "Use up to n threads.")

    CLI.add_argument(
        "--markup"
        , choices = ["reST", "kernel-doc", "auto"]
        , default = "auto"
        , help    = (
            "Markup of the comments. Change this option only if you know"
            " what you do. New comments must be marked up with reST!"))

    CMD = CLI.parse_args()

    if not CMD.srctree.EXISTS:
        ERR("%s does not exists." % CMD.srctree)
        sys.exit(42)

    if not CMD.srctree.ISDIR:
        ERR("%s is not a folder." % CMD.srctree)
        sys.exit(42)

    if not CMD.force and CMD.doctree.EXISTS:
        ERR("%s is in the way, remove it first" % CMD.doctree)
        sys.exit(42)

    CMD.rst_files = set()
    if CMD.markup == "auto":
        CMD.rst_files = docgrep(SRCTREE/"Documentation")

    pool = multiprocessing.Pool(CMD.threads)
    pool.map(autodoc_file, gather_filenames(CMD))
    pool.close()
    pool.join()

    insert_index_files(CMD.doctree)

# ------------------------------------------------------------------------------
def gather_filenames(cmd):
# ------------------------------------------------------------------------------
    MSG("gather files ...")

    for fname in cmd.srctree.reMatchFind(r"^.*\.[ch]$"):
        if fname.startswith(CMD.srctree/"Documentation"):
            continue
        yield fname

# ------------------------------------------------------------------------------
def autodoc_file(fname):
# ------------------------------------------------------------------------------

    fname  = fname.relpath(CMD.srctree)
    markup = CMD.markup

    if CMD.markup == "kernel-doc" and fname in CMD.rst_files:
        markup = "reST"

    opts = kerneldoc.ParseOptions(
        rel_fname       = fname
        , src_tree      = CMD.srctree
        , verbose_warn  = not (CMD.sloppy)
        , markup        = markup )

    parser = kerneldoc.Parser(opts, kerneldoc.NullTranslator())
    try:
        parser.parse()
    except Exception: # pylint: disable=W0703
        FATAL("kernel-doc markup of %s seems buggy / can't parse" % opts.fname)
        return

    if not parser.ctx.dump_storage:
        # no kernel-doc comments found
        MSG("parsed: NONE comments: %s" % opts.fname)
        return

    MSG("parsed: %4d comments: %s" % (len(parser.ctx.dump_storage), opts.fname))

    try:
        rst = six.StringIO()
        translator = kerneldoc.ReSTTranslator()
        opts.out   = rst

        # First try to output reST, this might fail, because the kernel-doc
        # parser part is to tollerant ("bad lines", "function name and function
        # declaration are different", etc ...).
        parser.parse_dump_storage(translator=translator)

        outFile = CMD.doctree / fname.replace(".","_") + ".rst"
        outFile.DIRNAME.makedirs()
        with outFile.openTextFile(mode="w") as out:
            out.write(rst.getvalue())

    except Exception: # pylint: disable=W0703
        FATAL("kernel-doc markup of %s seems buggy / can't parse" % opts.fname)
        return

# ------------------------------------------------------------------------------
def insert_index_files(folder):
# ------------------------------------------------------------------------------

    for folder, dirnames, filenames in folder.walk():
        ctx = kerneldoc.Container( title = folder.FILENAME )
        dirnames.sort()
        filenames.sort()
        indexFile = folder / "index.rst"
        MSG("create index: %s" % indexFile)
        with indexFile.openTextFile(mode="w") as index:
            index.write(TEMPLATE_INDEX % ctx)
            for d in dirnames:
                index.write("    %s/index\n" % d.FILENAME)
            for f in filenames:
                if f.FILENAME == "index":
                    continue
                index.write("    %s\n" % f.FILENAME)

# ------------------------------------------------------------------------------
def docgrep(folder):
# ------------------------------------------------------------------------------

    # Hackisch script to grep all '.. kernel-doc::' directives. The assumption
    # is, that comments from the source files used in those directives are
    # allready migratetd to the reST format. I guess that (ATM) 95-99% of the
    # comments are not migrated, those will be parsed with the old kernel-doc
    # comment style introduced by the old DocBook toolchain.

    pat = re.compile(r"^\s*\.\.\s+kernel-doc::\s*([^\s]+)\s*$")
    out = set()
    for rstFile in folder.reMatchFind(r".*\.rst"):
        if rstFile.BASENAME in IGNORE:
            continue
        #print(rstFile)
        with rstFile.openTextFile() as f:
            for l in f:
                match = pat.search(l)
                if match:
                    #print(match.group(1))
                    out.add(match.group(1))
    return sorted(out)
