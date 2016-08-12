# -*- coding: utf-8; mode: python -*-
u"""
    parseheaders
    ~~~~~~~~~~~~

    Implementation of the ``parse-header`` reST-directive.

    :copyright:  Copyright (C) 2016  Markus Heiser
    :license:    GPL Version 2, June 1991 see Linux/COPYING for details.

    The ``parse-header`` (:py:class:`ParseHeader`) directive includes contend
    from Linux kernel header files. The python side of this feature is only an
    adapter of the ``parse-headers.pl`` Perl script. This script parses a header
    file and converts it into a parsed-literal block, creating references for
    ioctls, defines, typedefs, enums and structs. It also allow an external file
    to modify the rules, in order to fix the expressions.  Usage of the Perl
    script::

      parse-headers.pl <header-file> [<exceptions-file>]

    Overview of directive's argument and options.

    .. code-block:: rst

        .. parse-header:: <header-file>
            :exceptions: <exceptions-file>
            :debug:

    The argument ``<header-file>`` is required, it points to a source file in the
    kernel source tree. The pathname is relative to kernel's root folder.  The
    options have the following meaning:

    ``exceptions <exceptions-file>``

      Pathname of file where the *exceptions* are defined. The pathname is
      relative to the reST file contain the parse-header directive.  At this
      time the Perl script supports two kinds of *exceptions*. These are
      *ignore* and *replace*.

      * ignore [ioctl|define|typedef|enum|struct|symbol]
      * replace [ioctl|define|typedef|enum|struct|symbol]

      Here are some examples for those::

        # Ignore header name
        ignore define _DVBAUDIO_H_

        # Typedef pointing to structs
        replace typedef audio_karaoke_t audio-karaoke

    ``debug``
      Inserts a code-block with the generated reST source. Sometimes it be
      helpful to see what reST is generated.

    .. note::

       The parse-headers directive uses the ``kerneldoc_srctree`` setting from
       the kernel-doc directive and adds a new config value ``parseheader_bin``
       pointing to the ``parse-headers.pl`` Perl script.

"""

# ==============================================================================
# imports
# ==============================================================================

import sys
from os import path
import subprocess

# Since Sphinx 1.2 does not require six, we can't assume that six is installed
# from six impot text_type

from docutils import nodes
from docutils.parsers.rst import Directive, directives
from docutils.statemachine import ViewList
from docutils.utils.error_reporting import ErrorString

from sphinx.ext.autodoc import AutodocReporter

# ==============================================================================
# common globals
# ==============================================================================

__version__  = '1.0'

# We can't assume that six is installed
PY3 = sys.version_info[0] == 3
PY2 = sys.version_info[0] == 2
if PY3:
    # pylint: disable=C0103, W0622
    unicode     = str
    basestring  = str

# ==============================================================================
def setup(app):
# ==============================================================================

    app.add_directive("parse-header", ParseHeader)
    app.add_config_value(
        'parseheader_bin'
        , path.join(path.dirname(__file__), 'parse-headers.pl')
        , 'env')

    return dict(
        version = __version__
        , parallel_read_safe = True
        , parallel_write_safe = True
    )

# ==============================================================================
class ParseHeader(Directive):
# ==============================================================================

    u"""ParseHeader (``parse-header``) directive"""

    required_arguments = 1
    optional_arguments = 0
    has_content = False
    final_argument_whitespace = True

    option_spec = {
        "exceptions" : directives.unchanged
        , "debug"    : directives.flag     # insert generated reST as code-block
    }

    def run(self):
        env   = self.state.document.settings.env
        doc   = self.state.document
        fname = env.config.kerneldoc_srctree + '/' + self.arguments[0]
        env.note_dependency(path.abspath(fname))

        if not doc.settings.file_insertion_enabled:
            raise self.warning("docutils: file insertion disabled")

        cmd = [ env.config.parseheader_bin, fname ]
        if "exceptions" in self.options:
            cmd.append(
                path.join(path.dirname(doc.current_source)
                          , self.options.get("exceptions"))
            )

        lines = self.runCmd(cmd)
        nodeList = self.nestedParse(lines, fname)
        return nodeList

    def runCmd(self, cmd, **kwargs):
        u"""Run command ``cmd`` and return it's stdout as unicode."""

        try:
            proc = subprocess.Popen(
                cmd
                , stdout = subprocess.PIPE
                , universal_newlines = True
                , **kwargs
            )
            out, _none = proc.communicate()

            if proc.returncode != 0:
                raise self.severe(
                    u"command '%s' failed with return code %d"
                    % (" ".join(cmd), proc.returncode)
                )
        except OSError as exc:
            raise self.severe(u"problems with '%s' directive: %s."
                              % (self.name, ErrorString(exc)))
        return unicode(out)

    def nestedParse(self, lines, fname):
        content = ViewList()
        node    = nodes.section()

        if "debug" in self.options:
            code_block = "\n\n.. code-block:: rst\n    :linenos:\n"
            for l in lines.split("\n"):
                code_block += "\n    " + l
            lines = code_block + "\n\n"

        for c, l in enumerate(lines.split("\n")):
            content.append(l, fname, c)

        buf  = self.state.memo.title_styles, self.state.memo.section_level, self.state.memo.reporter
        self.state.memo.title_styles  = []
        self.state.memo.section_level = 0
        self.state.memo.reporter      = AutodocReporter(content, self.state.memo.reporter)
        try:
            self.state.nested_parse(content, 0, node, match_titles=1)
        finally:
            self.state.memo.title_styles, self.state.memo.section_level, self.state.memo.reporter = buf
        return node.children
