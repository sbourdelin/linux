/*
 * Copyright (C) 2015 Nobuo Iwata
 *
 * This is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307,
 * USA.
 */

#include "USBWSWebSocket.h"
#include "USBWSUtil.h"
#include "Poco/Exception.h"
#include "Poco/Net/NetException.h"

extern "C" {
#include "usbip_common.h"
}

using namespace ::Poco::Net;
using namespace ::Usbip::WebSock::Poco;

USBWSWebSocket::USBWSWebSocket(
	HTTPServerRequest& req,
	HTTPServerResponse& rsp,
	int pingPong) :
	WebSocket(req, rsp),
	Timer(0, pingPong * 1000),
	fApp(0),
	fPingPongStarted(false),
	fFirstTimeout(true),
	fSendLock()
{
	setKeepAlive(true);
	if (pingPong > 0) {
        	setReceiveTimeout(Timespan(pingPong + 60, 0));
		startPingPong();
		fPingPongStarted = true;
	}	
}

USBWSWebSocket::USBWSWebSocket(
	HTTPClientSession& cs,
	HTTPRequest& req,
	HTTPResponse& rsp,
	int pingPong) :
	WebSocket(cs, req, rsp),
	Timer(0, 0),
	fApp(0),
	fPingPongStarted(false),
	fFirstTimeout(true),
	fSendLock()
{
	setKeepAlive(true);
	if (pingPong > 0) {
        	setReceiveTimeout(Timespan(pingPong + 60, 0));
	}	
}

ssize_t USBWSWebSocket::send(void *arg, void *buf, size_t len)
{
	USBWSWebSocket *ws = (USBWSWebSocket*)arg;
	return ws->send(buf, len);
}

ssize_t USBWSWebSocket::recv(void *arg, void *buf, size_t len, int all)
{
	USBWSWebSocket *ws = (USBWSWebSocket*)arg;
	return ws->recv(buf, len, all);
}

void USBWSWebSocket::shutdown(void *arg)
{
	USBWSWebSocket *ws = (USBWSWebSocket*)arg;
	ws->shutdown();
}

ssize_t USBWSWebSocket::send(void *buf, size_t len)
{
	ssize_t ret;
	fSendLock.lock();
	try {
		ret = sendFrame(buf, len, WebSocket::FRAME_OP_BINARY);
	} catch (::Poco::IOException& e) {
		dbg("Send IOException %s", e.message().c_str());
		ret = -1;
		errno = EIO;
	}
	fSendLock.unlock();
	return ret;
}

ssize_t USBWSWebSocket::recv(void *buf, size_t len, int all)
{
	char *p = (char*)buf;
	try {
		ssize_t received = 0;
		int op, flags;
        	do {
                	ssize_t bytes =
				receiveFrame(p+received, len-received, flags);
			op = flags & WebSocket::FRAME_OP_BITMASK;
			if (op == WebSocket::FRAME_OP_BINARY) {
				if (bytes == 0) {
					return received;
				}
				received += bytes;
			} else if (op == WebSocket::FRAME_OP_PING) {
				sendPong();
			} else if (op == WebSocket::FRAME_OP_PONG) {
				restartPingPong();
			} else if (op == WebSocket::FRAME_OP_CLOSE) {
				throw ::Poco::IOException(
					"Recieved close frame");
			} else {
				throw ::Poco::Net::WebSocketException(
					"Unsupported op code:" +
					USBWSUtil::i2s(op));
			}	
        	} while((all && received < len) ||
			op != WebSocket::FRAME_OP_BINARY);
		restartPingPong();
		return received;
	} catch (::Poco::Net::WebSocketException& e) {
		dbg("Recv WebSocketException %s", e.message().c_str());
		errno = EINVAL;
	} catch (::Poco::TimeoutException& e) {
		dbg("Recv TimeoutException %s", e.message().c_str());
		errno = ETIMEDOUT;
	} catch (::Poco::Net::NetException& e) {
		dbg("Recv NetException %s", e.message().c_str());
		errno = EIO;
	} catch (::Poco::IOException& e) {
		dbg("Recv IOException %s", e.message().c_str());
		errno = EIO;
	}
	return -1;
}

void USBWSWebSocket::shutdown(void)
{
	dbg("Shutting down websocket.");
	fPingPongStarted = false;
	shutdownReceive();
	close();
}

void USBWSWebSocket::startPingPong(void)
{
	TimerCallback<USBWSWebSocket> callback(*this, &USBWSWebSocket::onTimer);
	start(callback);
}

void USBWSWebSocket::restartPingPong(void)
{
	if (fPingPongStarted) {
		restart();
	}
}

void USBWSWebSocket::onTimer(Timer& timer)
{
	USBWSWebSocket& ws = (USBWSWebSocket&)timer; 
	if (ws.fFirstTimeout) {
		ws.fFirstTimeout = false;
	} else {
		ws.sendPing();
	}
}

void USBWSWebSocket::sendPing()
{
	if (fPingPongStarted) {
		dbg("Ping");
		sendPingPong(WebSocket::FRAME_OP_PING);
	}
}

void USBWSWebSocket::sendPong(void)
{
	dbg("Pong");
	sendPingPong(WebSocket::FRAME_OP_PONG);
}

void USBWSWebSocket::sendPingPong(int op)
{
	fSendLock.lock();
	try {
		sendFrame(NULL, 0, op);
	} catch (::Poco::IOException& e) {
		dbg("Send IOException %s", e.message().c_str());
	}
	fSendLock.unlock();
}

