#!/usr/grte/v4/bin/python2.7
import unittest
import os
import socket
import shutil
import multiprocessing

cgroup_net_root = '/dev/cgroup/net'

def create_cgroup(name):
    '''
    Creates a cgroup with the given name. The name should also include the names
    of all ancestors separated by slashes, such as 'a/b/c'. Returns a path to
    the directory of the newly created cgroup.
    '''
    cgroup_dir = os.path.join(cgroup_net_root, name)
    while True:
        try:
            os.mkdir(cgroup_dir)
            break
        except OSError as e:
            # remove it if it already exists, then try to create again
            # there will be errors when rmtree tries to remove the cgroup files,
            # but these errors should be ignored because we only care about
            # rmdir'ing the directories, which will automatically get rid of the
            # files inside them
            shutil.rmtree(cgroup_dir, ignore_errors=True)

    return cgroup_dir


def parse_ranges(ranges_str):
    '''
    Converts a range string like "100-200,300-400" into a set of 2-tuples like
    {(100,200),(300,400)}.
    '''
    return set(tuple(int(l) for l in r.strip().split('-'))
               for r in ranges_str.split(','))

def acquire_udp_ports(cgroup_dir, e2, n, addr, numfailq):
    '''
    Waits for the event e1, attempts to acquire n udp ports connected to addr,
    and then puts the number of failures on the queue and waits for e2. Then,
    all sockets are closed. (Intended to be called as a subprocess.)

    While waiting for e1, the parent process can set this process's cgroup.
    While waiting for e2, the parent process can read the udp statistics while
    this process is still alive.
    '''

    with open(os.path.join(cgroup_dir, 'tasks'), 'w') as f:
        # add proc1 to cgroup a/b
        f.write(str(os.getpid()))

    socketset = set()
    numfail = 0
    for _ in xrange(n):
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        try:
            s.connect(addr)
        except socket.error as e:
            numfail += 1
        socketset.add(s)

    if numfailq is not None:
        numfailq.put(numfail)

    if e2 is not None:
        e2.wait()

    for s in socketset:
        s.close()


class NetCgroupTest(unittest.TestCase):

    def test_port_range_check(self):
        '''
        Test that the kernel is correctly checking that a port is in the
        relevant range when a process in a net cgroup is trying to bind a socket
        to it, or trying to listen on a socket bound to it.
        '''

        # create a new cgroup a
        cgroup_a_dir = create_cgroup('a')

        # add current process to cgroup a
        with open(os.path.join(cgroup_a_dir, 'tasks'), 'w') as f:
            f.write(str(os.getpid()))

        # set bind and listen range of cgroup a
        with open(os.path.join(cgroup_a_dir, 'net.bind_port_ranges'), 'w') as f:
            f.write('300-400,500')
        with open(os.path.join(cgroup_a_dir, 'net.listen_port_ranges'), 'w') as f:
            f.write('350-400')

        # try binding and listening on various ports, and check if they succeed
        # or fail appropriately
        s = socket.socket()
        try:
            s.bind(('0.0.0.0', 350)) # should bind and listen successfully
            try:
                s.listen(5)
            except socket.error:
                self.fail('unexpectedly failed to listen')
        except socket.error:
            self.fail('unexpectedly failed to bind')
        s.close()

        s = socket.socket()
        try:
            s.bind(('0.0.0.0', 370)) # should bind and listen successfully
            try:
                s.listen(5)
            except socket.error:
                self.fail('unexpectedly failed to listen')
        except socket.error:
            self.fail('unexpectedly failed to bind')
        s.close()

        s = socket.socket()
        try:
            s.bind(('0.0.0.0', 320)) # should bind successfully but fail to listen
            with self.assertRaises(socket.error):
                s.listen(5)
        except socket.error:
            self.fail('unexpectedly failed to bind')
        s.close()

        s = socket.socket()
        try:
            s.bind(('0.0.0.0', 500)) # should bind successfully but fail to listen
            with self.assertRaises(socket.error):
                s.listen(5)
        except socket.error:
            self.fail('unexpectedly failed to bind')
        s.close()

        s = socket.socket()
        with self.assertRaises(socket.error):
            s.bind(('0.0.0.0', 200)) # should fail to bind
        s.close()

        s = socket.socket()
        with self.assertRaises(socket.error):
            s.bind(('0.0.0.0', 401)) # should fail to bind
        s.close()

        # remove current process from cgroup a (by adding it to root)
        with open(os.path.join(cgroup_net_root, 'tasks'), 'w') as f:
            f.write(str(os.getpid()))


    def test_range_inheritance(self):
        '''
        Test that the kernel copies the ranges from parent when a net cgroup is
        created.
        '''
        cgroup_a_dir = create_cgroup('a')

        # set ranges of parent
        with open(os.path.join(cgroup_a_dir, 'net.bind_port_ranges'), 'w') as f:
            f.write('100-200,300-400,500')
        with open(os.path.join(cgroup_a_dir, 'net.listen_port_ranges'), 'w') as f:
            f.write('150,300')

        cgroup_b_dir = create_cgroup('a/b')

        # check that bind range is the same in both a and a/b
        with open(os.path.join(cgroup_a_dir, 'net.bind_port_ranges')) as fa, \
             open(os.path.join(cgroup_b_dir, 'net.bind_port_ranges')) as fb:
            ranges_a_str = fa.read()
            ranges_b_str = fb.read()
            ranges_a = parse_ranges(ranges_a_str)
            ranges_b = parse_ranges(ranges_b_str)
            self.assertEqual(ranges_a, ranges_b)

        # check that listen range is the same in both a and a/b
        with open(os.path.join(cgroup_a_dir, 'net.listen_port_ranges')) as fa, \
             open(os.path.join(cgroup_b_dir, 'net.listen_port_ranges')) as fb:
            ranges_a_str = fa.read()
            ranges_b_str = fb.read()
            ranges_a = parse_ranges(ranges_a_str)
            ranges_b = parse_ranges(ranges_b_str)
            self.assertEqual(ranges_a, ranges_b)

    def test_enforce_subset(self):
        '''
        Test that the kernel enforces the rule that a cgroup cannot have a port
        within its range that isn't in its parent's range.
        '''
        cgroup_a_dir = create_cgroup('a')

        # set ranges of parent
        with open(os.path.join(cgroup_a_dir, 'net.bind_port_ranges'), 'w') as f:
            f.write('100-200,300-400,500')
        with open(os.path.join(cgroup_a_dir, 'net.listen_port_ranges'), 'w') as f:
            f.write('150,300')

        cgroup_b_dir = create_cgroup('a/b')

        # try to set a/b ranges to various things

        with open(os.path.join(cgroup_b_dir, 'net.bind_port_ranges'), 'w') as f:
            try:
                f.write('130-160,500') # should succeed
            except IOError as e:
                self.fail('unexpectedly failed to set ranges')
        with open(os.path.join(cgroup_b_dir, 'net.listen_port_ranges'), 'w') as f:
            try:
                f.write('150') # should succeed
            except IOError as e:
                self.fail('unexpectedly failed to set ranges')

        with open(os.path.join(cgroup_b_dir, 'net.bind_port_ranges'), 'w') as f:
            with self.assertRaises(IOError):
                f.write('200-300,350,360-370') # should fail
        with open(os.path.join(cgroup_b_dir, 'net.listen_port_ranges'), 'w') as f:
            with self.assertRaises(IOError):
                f.write('200-300') # should fail

        with open(os.path.join(cgroup_b_dir, 'net.bind_port_ranges'), 'w') as f:
            with self.assertRaises(IOError):
                f.write('210,220,230-240') # should fail
        with open(os.path.join(cgroup_b_dir, 'net.listen_port_ranges'), 'w') as f:
            with self.assertRaises(IOError):
                f.write('210,220,230-240') # should fail

    def test_udp_usage(self):
        '''
        Tests if the kernel counts udp usage in a hierarchical manner.
        '''

        # create a new cgroups a, a/b, a/c
        cgroup_a_dir = create_cgroup('a')
        cgroup_b_dir = create_cgroup('a/b')
        cgroup_c_dir = create_cgroup('a/c')

        # event for synchronizing subprocesses
        e3 = multiprocessing.Event()

        # create a server socket so that subprocesses can connect to it
        addr = ('0.0.0.0', 3000)
        serversocket = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        serversocket.bind(addr)

        numfailq1 = multiprocessing.Queue()
        numfailq2 = multiprocessing.Queue()

        # create 2 subprocesses, one going into a/b, other going into a/c
        proc1 = multiprocessing.Process(name='proc1', target=acquire_udp_ports,
                                        args=(cgroup_b_dir, e3, 4, addr, numfailq1))
        proc2 = multiprocessing.Process(name='proc2', target=acquire_udp_ports,
                                        args=(cgroup_c_dir, e3, 3, addr, numfailq2))

        # proc1 will acquire 4 ports in cgroup a/b
        proc1.start()
        # make sure none failed
        self.assertEqual(numfailq1.get(), 0)

        # proc2 will acquire 3 ports in cgroup a/c
        proc2.start()
        # make sure none failed
        self.assertEqual(numfailq2.get(), 0)

        # check if the usage count is correct
        with open(os.path.join(cgroup_a_dir, 'net.udp_usage')) as f:
            # a/net.udp_usage should be 7, because it counts its children's too
            self.assertEqual(int(f.read()), 7)
        with open(os.path.join(cgroup_b_dir, 'net.udp_usage')) as f:
            self.assertEqual(int(f.read()), 4)
        with open(os.path.join(cgroup_c_dir, 'net.udp_usage')) as f:
            self.assertEqual(int(f.read()), 3)

        # signal the subprocesses that they can close their sockets now
        e3.set()
        serversocket.close()

    def test_udp_limit(self):
        '''
        Test if the kernel only allows processes to use a limited number of udp
        ports.
        '''
        # create a new cgroup a
        cgroup_a_dir = create_cgroup('a')

        # event for synchronizing subprocess
        e2 = multiprocessing.Event()

        # create a server socket so that subprocesses can connect to it
        addr = ('0.0.0.0', 3000)
        serversocket = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        serversocket.bind(addr)

        # queue for getting fail count
        numfailq = multiprocessing.Queue()

        # create and start a subprocess
        proc = multiprocessing.Process(name='proc', target=acquire_udp_ports,
                                       args=(cgroup_a_dir, e2, 8, addr, numfailq))

        # set the udp limit to 5
        with open(os.path.join(cgroup_a_dir, 'net.udp_limit'), 'w') as f:
            f.write('5')

        # proc will attempt to acquire 8 udp port (only 5 will be successful)
        proc.start()
        # make sure that there were indeed 3 failures
        self.assertEqual(numfailq.get(), 3)

        # check if the usage count and fail count in the files is correct
        with open(os.path.join(cgroup_a_dir, 'net.udp_usage')) as f:
            self.assertEqual(int(f.read()), 5)
        with open(os.path.join(cgroup_a_dir, 'net.udp_failcnt')) as f:
            self.assertEqual(int(f.read()), 3)

        # signal the subprocess that it can close its sockets now
        e2.set()
        serversocket.close()

    def test_dscp_range_check(self):
        # create a new cgroup a
        cgroup_a_dir = create_cgroup('a')

        # add current process to cgroup a
        with open(os.path.join(cgroup_a_dir, 'tasks'), 'w') as f:
            f.write(str(os.getpid()))

        # set dscp range of cgroup a
        with open(os.path.join(cgroup_a_dir, 'net.dscp_ranges'), 'w') as f:
            f.write('20-40')

        # try setting the IP_TOS option to various things
        s = socket.socket()
        try:
            # dscp = 30 (in range)
            s.setsockopt(socket.IPPROTO_IP, socket.IP_TOS, 30 << 2 | 1)
        except socket.error:
            self.fail('unexpectedly failed to setsockopt')
        s.close()

        s = socket.socket()
        with self.assertRaises(socket.error):
            # dscp = 50 (out of range)
            s.setsockopt(socket.IPPROTO_IP, socket.IP_TOS, 50 << 2 | 2)
        s.close()

        s = socket.socket()
        with self.assertRaises(socket.error):
            # dscp = 10 (out of range)
            s.setsockopt(socket.IPPROTO_IP, socket.IP_TOS, 10 << 2 | 3)
        s.close()

        # remove current process from cgroup a (by adding it to root)
        with open(os.path.join(cgroup_net_root, 'tasks'), 'w') as f:
            f.write(str(os.getpid()))

if __name__ == '__main__':
    unittest.main()
