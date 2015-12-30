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

#include "USBWSRequestHandler.h"
#include "USBWSWebSocket.h"
#include "USBWSUtil.h"
#include "Poco/Net/Socket.h"
#include "Poco/Net/NetException.h"
extern "C" {
#include "usbip_common.h"
#include "usbipd.h"
}

using namespace ::Poco;
using namespace ::Poco::Net;
using namespace ::Usbip::WebSock::Poco;

USBWSRequestHandler::USBWSRequestHandler(int pingPong, Logger& logger) :
	HTTPRequestHandler(),
	fPingPong(pingPong),
	fLogger(logger)
{
}

USBWSRequestHandler::~USBWSRequestHandler()
{
}

void USBWSRequestHandler::handleRequest(
	HTTPServerRequest& req,
	HTTPServerResponse& rsp)
{
	int interrupted = 0;
	fLogger.information("WebSocket connection established.");
	try {
		USBWSWebSocket ws(req, rsp, fPingPong);

		usbip_sock_t sock;
		usbip_sock_init(&sock, ws.getSockfd(), &ws,
			USBWSWebSocket::send, USBWSWebSocket::recv,
			USBWSWebSocket::shutdown);

		SocketAddress addr = ws.address();
		std::string port = USBWSUtil::i2s(addr.port());

		fLogger.information("Entering to usbip_recv_pdu().");
		usbip_recv_pdu(&sock,
			addr.host().toString().c_str(),
			port.c_str());
		fLogger.information("Exited from usbip_recv_pdu().");
	} catch (WebSocketException& e) {
		fLogger.information("WebSocketexception.");
		fLogger.log(e);
		switch(e.code()) {
		case WebSocket::WS_ERR_HANDSHAKE_UNSUPPORTED_VERSION:
			rsp.set("Sec-WebSocket-Version", WebSocket::WEBSOCKET_VERSION);
			// fallthrough
		case WebSocket::WS_ERR_NO_HANDSHAKE:
		case WebSocket::WS_ERR_HANDSHAKE_NO_VERSION:
		case WebSocket::WS_ERR_HANDSHAKE_NO_KEY:
			rsp.setContentLength(0);
			rsp.send();
			break;
		case WebSocket::WS_ERR_HANDSHAKE_ACCEPT:
		case WebSocket::WS_ERR_UNAUTHORIZED:
		case WebSocket::WS_ERR_PAYLOAD_TOO_BIG:
		case WebSocket::WS_ERR_INCOMPLETE_FRAME:
		default:
			break;
		}
	}
	fLogger.information("WebSocket connection terminated.");
}

