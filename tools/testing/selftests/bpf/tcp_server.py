#!/usr/local/bin/python
#
# SPDX-License-Identifier: GPL-2.0
#

import sys, os, os.path, getopt
import socket, time
import subprocess
import select

def read(sock, n):
    buf = ''
    while len(buf) < n:
        rem = n - len(buf)
        try: s = sock.recv(rem)
        except (socket.error), e: return ''
        buf += s
    return buf

def send(sock, s):
    total = len(s)
    count = 0
    while count < total:
        try: n = sock.send(s)
        except (socket.error), e: n = 0
        if n == 0:
            return count;
        count += n
    return count


SERVER_PORT = 12877
MAX_PORTS = 2

serverPort = SERVER_PORT
serverSocket = None

HostName = socket.gethostname()

# create passive socket
serverSocket = socket.socket(socket.AF_INET6, socket.SOCK_STREAM)
host = socket.gethostname()

while serverPort < SERVER_PORT + 5:
	try: serverSocket.bind((host, serverPort))
	except socket.error as msg:
            serverPort += 1
            continue
	break

cmdStr = ("./tcp_client.py %d &") % (serverPort)
os.system(cmdStr)

buf = ''
n = 0
while n < 500:
    buf += '.'
    n += 1

serverSocket.listen(MAX_PORTS)
readList = [serverSocket]

while True:
    readyRead, readyWrite, inError = \
        select.select(readList, [], [], 10)

    if len(readyRead) > 0:
        waitCount = 0
        for sock in readyRead:
            if sock == serverSocket:
                (clientSocket, address) = serverSocket.accept()
                address = str(address[0])
                readList.append(clientSocket)
            else:
                s = read(sock, 1000)
                n = send(sock, buf)
                sock.close()
                time.sleep(1)
                sys.exit(0)
