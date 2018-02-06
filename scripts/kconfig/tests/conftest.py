# SPDX-License-Identifier: GPL-2.0
#
# Copyright (C) 2018 Masahiro Yamada <yamada.masahiro@socionext.com>
#

import os
import pytest
import shutil
import subprocess
import tempfile

conf_path = os.path.abspath(os.path.join('scripts', 'kconfig', 'conf'))

class Conf:

    def __init__(self, request):
        """Create a new Conf object, which is a scripts/kconfig/conf
        runner and result checker.

        Arguments:
        request - object to introspect the requesting test module
        """

        # the directory of the test being run
        self.test_dir = os.path.dirname(str(request.fspath))

    def __run_conf(self, mode, dot_config=None, out_file='.config',
                   interactive=False, in_keys=None, extra_env={}):
        """Run scripts/kconfig/conf

        mode: input mode option (--oldaskconfig, --defconfig=<file> etc.)
        dot_config: the .config file for input.
        out_file: file name to contain the output config data.
        interactive: flag to specify the interactive mode.
        in_keys: key inputs for interactive modes.
        extra_env: additional environment.
        """

        command = [conf_path, mode, 'Kconfig']

        # Override 'srctree' environment to make the test as the top directory
        extra_env['srctree'] = self.test_dir

        # scripts/kconfig/conf is run in a temporary directory.
        # This directory is automatically removed when done.
        with tempfile.TemporaryDirectory() as temp_dir:

            # if .config is given, copy it to the working directory
            if dot_config:
                shutil.copyfile(os.path.join(self.test_dir, dot_config),
                                os.path.join(temp_dir, '.config'))

            ps = subprocess.Popen(command,
                                  stdin=subprocess.PIPE,
                                  stdout=subprocess.PIPE,
                                  stderr=subprocess.PIPE,
                                  cwd=temp_dir,
                                  env=dict(os.environ, **extra_env))

            # If user key input is specified, feed it into stdin.
            if in_keys:
                ps.stdin.write(in_keys.encode('utf-8'))

            while ps.poll() == None:
                # For interactive modes such as 'make config', 'make oldconfig',
                # send 'Enter' key until the program finishes.
                if interactive:
                    ps.stdin.write(b'\n')

            self.retcode = ps.returncode
            self.stdout = ps.stdout.read().decode()
            self.stderr = ps.stderr.read().decode()

            # Retrieve the resulted config data only when .config is supposed
            # to exist.  If the command fails, the .config does not exist.
            # 'make listnewconfig' does not produce .config in the first place.
            if self.retcode == 0 and out_file:
                with open(os.path.join(temp_dir, out_file)) as f:
                    self.config = f.read()
            else:
                self.config = None

        # Logging:
        # Pytest captures the following information by default.  In failure
        # of tests, the captured log will be displayed.  This will be useful to
        # figure out what has happened.

        print("command: {}\n".format(' '.join(command)))
        print("retcode: {}\n".format(self.retcode))

        if dot_config:
            print("input .config:".format(dot_config))

        print("stdout:")
        print(self.stdout)
        print("stderr:")
        print(self.stderr)

        if self.config is not None:
            print("output of {}:".format(out_file))
            print(self.config)

        return self.retcode

    def oldaskconfig(self, dot_config=None, in_keys=None):
        """Run oldaskconfig (make config)

        dot_config: the .config file for input (optional).
        in_key: key inputs (optional).
        """
        return self.__run_conf('--oldaskconfig', dot_config=dot_config,
                             interactive=True, in_keys=in_keys)

    def oldconfig(self, dot_config=None, in_keys=None):
        """Run oldconfig

        dot_config: the .config file for input (optional).
        in_key: key inputs (optional).
        """
        return self.__run_conf('--oldconfig', dot_config=dot_config,
                             interactive=True, in_keys=in_keys)

    def defconfig(self, defconfig):
        """Run defconfig

        defconfig: the defconfig file for input.
        """
        defconfig_path = os.path.join(self.test_dir, defconfig)
        return self.__run_conf('--defconfig={}'.format(defconfig_path))

    def olddefconfig(self, dot_config=None):
        """Run olddefconfig

        dot_config: the .config file for input (optional).
        """
        return self.__run_conf('--olddefconfig', dot_config=dot_config)

    def __allconfig(self, foo, all_config):
        """Run all*config

        all_config: fragment config file for KCONFIG_ALLCONFIG (optional).
        """
        if all_config:
            all_config_path = os.path.join(self.test_dir, all_config)
            extra_env = {'KCONFIG_ALLCONFIG': all_config_path}
        else:
            extra_env = {}

        return self.__run_conf('--all{}config'.format(foo), extra_env=extra_env)

    def allyesconfig(self, all_config=None):
        """Run allyesconfig
        """
        return self.__allconfig('yes', all_config)

    def allmodconfig(self, all_config=None):
        """Run allmodconfig
        """
        return self.__allconfig('mod', all_config)

    def allnoconfig(self, all_config=None):
        """Run allnoconfig
        """
        return self.__allconfig('no', all_config)

    def alldefconfig(self, all_config=None):
        """Run alldefconfig
        """
        return self.__allconfig('def', all_config)

    def savedefconfig(self, dot_config):
        """Run savedefconfig
        """
        return self.__run_conf('--savedefconfig', out_file='defconfig')

    def listnewconfig(self, dot_config=None):
        """Run listnewconfig
        """
        return self.__run_conf('--listnewconfig', dot_config=dot_config,
                               out_file=None)

    # checkers
    def __read_and_compare(self, compare, expected):
        """Compare the result with expectation.

        Arguments:
        compare: function to compare the result with expectation
        expected: file that contains the expected data
        """
        with open(os.path.join(self.test_dir, expected)) as f:
            expected_data = f.read()
        print(expected_data)
        return compare(self, expected_data)

    def __contains(self, attr, expected):
        print("{0} is expected to contain '{1}':".format(attr, expected))
        return self.__read_and_compare(lambda s, e: getattr(s, attr).find(e) >= 0,
                                       expected)

    def __matches(self, attr, expected):
        print("{0} is expected to match '{1}':".format(attr, expected))
        return self.__read_and_compare(lambda s, e: getattr(s, attr) == e,
                                       expected)

    def config_contains(self, expected):
        """Check if resulted configuration contains expected data.

        Arguments:
        expected: file that contains the expected data.
        """
        return self.__contains('config', expected)

    def config_matches(self, expected):
        """Check if resulted configuration exactly matches expected data.

        Arguments:
        expected: file that contains the expected data.
        """
        return self.__matches('config', expected)

    def stdout_contains(self, expected):
        """Check if resulted stdout contains expected data.

        Arguments:
        expected: file that contains the expected data.
        """
        return self.__contains('stdout', expected)

    def stdout_matches(self, cmp_file):
        """Check if resulted stdout exactly matches expected data.

        Arguments:
        expected: file that contains the expected data.
        """
        return self.__matches('stdout', expected)

    def stderr_contains(self, expected):
        """Check if resulted stderr contains expected data.

        Arguments:
        expected: file that contains the expected data.
        """
        return self.__contains('stderr', expected)

    def stderr_matches(self, cmp_file):
        """Check if resulted stderr exactly matches expected data.

        Arguments:
        expected: file that contains the expected data.
        """
        return self.__matches('stderr', expected)

@pytest.fixture(scope="module")
def conf(request):
    return Conf(request)
