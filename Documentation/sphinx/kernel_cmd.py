# -*- coding: utf-8; mode: python -*-
u"""
    kernel-cmd
    ~~~~~~~~~~

    Implementation of the ``kernel-cmd`` reST-directive.

    :copyright:  Copyright (C) 2016  Markus Heiser
    :license:    GPL Version 2, June 1991 see Linux/COPYING for details.

    The ``kernel-cmd`` (:py:class:`KernelCmd`) directive includes contend
    from the stdout of a comand-line.

    Overview of directive's argument and options.

    .. code-block:: rst

        .. kernel-cmd:: <command line>
            :depends: <list of comma separated file names>
            :code-block: <language>
            :debug:

    The argument ``<command line>`` is required, it is a command line to be
    executed. The stdout stream of the command is captured and is inserted as
    reST content. The command line is executed in a system shell where the PATH
    environment is extended with the paths ::

        PATH=$(srctree)/scripts:$(srctree)/Documentation/sphinx:...

    ``depends <list of comma separated file names>``

      If the stdout of the command line depends on files, you can add them as
      dependency, which means: if one of the listed files is changed, the
      sphinx-build (environment) is newly build.

    ``code-block <language>``

      If the called script outputs a code-block, use the ``code-block`` option
      with an *language* as argument.  The valid values for the highlighting
      language are:

      * none (no highlighting)
      * guess (let Pygments guess the lexer based on contents, only works
        with certain well-recognizable languages)
      * rest
      * c
      * and any other lexer alias that `Pygments
        <http://pygments.org/docs/lexers/>`_ supports.

    ``debug``
      Inserts a code-block with the *raw* reST. Sometimes it is helpful to see
      what reST is generated.

    .. warning::

       The kernel-cmd directive **executes** commands, whatever poses a risk
       (shell injection) in itself!

       The command might depend on local installations, don't use commands which
       are not available in some OS (be clear about the dependencies).

"""

import sys
import os
from os import path
import subprocess

from sphinx.ext.autodoc import AutodocReporter

from docutils import nodes
from docutils.parsers.rst import Directive, directives
from docutils.statemachine import ViewList
from docutils.utils.error_reporting import ErrorString


__version__  = '1.0'

# We can't assume that six is installed
PY3 = sys.version_info[0] == 3
PY2 = sys.version_info[0] == 2
if PY3:
    # pylint: disable=C0103, W0622
    unicode     = str
    basestring  = str

def setup(app):

    app.add_directive("kernel-cmd", KernelCmd)
    return dict(
        version = __version__
        , parallel_read_safe = True
        , parallel_write_safe = True
    )

class KernelCmd(Directive):

    u"""KernelCmd (``kernel-cmd``) directive"""

    required_arguments = 1
    optional_arguments = 0
    has_content = False
    final_argument_whitespace = True

    option_spec = {
        "depends"   : directives.unchanged,
        "code-block": directives.unchanged,
        "debug"     : directives.flag
    }

    def warn(self, message, **replace):
        replace["fname"]   = self.state.document.current_source
        replace["line_no"] = replace.get("line_no", self.lineno)
        message = ("%(fname)s:%(line_no)s: [kernel-cmd WARN] : " + message) % replace
        self.state.document.settings.env.app.warn(message, prefix="")

    def run(self):

        doc = self.state.document
        if not doc.settings.file_insertion_enabled:
            raise self.warning("docutils: file insertion disabled")

        env = doc.settings.env
        cwd = path.dirname(doc.current_source)
        cmd = self.arguments[0]

        if "depends" in self.options:
            dep = self.options.get("depends")
            dep = ''.join([s.strip() for s in dep.splitlines()])
            dep = [s.strip() for s in dep.split(",")]
            for p in dep:
                env.note_dependency(p)

        srctree = path.abspath(os.environ["srctree"])

        # Since there is no *source* file, we use the command string as
        # (default) filename
        fname = cmd

        # extend PATH with $(srctree)/scripts:$(srctree)/Documentation/sphinx
        path_env = os.pathsep.join([
            srctree + os.sep + "scripts",
            srctree + os.sep + "Documentation" + os.sep + "sphinx",
            os.environ["PATH"]
        ])
        shell_env = os.environ.copy()
        shell_env["PATH"]    = path_env
        shell_env["srctree"] = srctree

        lines = self.runCmd(cmd, shell=True, cwd=cwd, env=shell_env)
        nodeList = self.nestedParse(lines, fname)
        return nodeList

    def runCmd(self, cmd, **kwargs):
        u"""Run command ``cmd`` and return it's stdout as unicode."""

        try:
            proc = subprocess.Popen(
                cmd
                , stdout = subprocess.PIPE
                , stderr = subprocess.PIPE
                , universal_newlines = True
                , **kwargs
            )
            out, err = proc.communicate()
            if err:
                self.warn(err)
            if proc.returncode != 0:
                raise self.severe(
                    u"command '%s' failed with return code %d"
                    % (cmd, proc.returncode)
                )
        except OSError as exc:
            raise self.severe(u"problems with '%s' directive: %s."
                              % (self.name, ErrorString(exc)))
        return unicode(out)

    def nestedParse(self, lines, fname):
        content = ViewList()
        node    = nodes.section()

        if "code-block" in self.options:
            code_block = "\n\n.. code-block:: %s\n" % self.options["code-block"]
            for l in lines.split("\n"):
                code_block += "\n    " + l
            lines = code_block + "\n\n"

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

