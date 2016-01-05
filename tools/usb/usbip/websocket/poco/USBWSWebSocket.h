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

#ifndef _USBIP_WEBSOCK_POCO_USBWS_WEBSOCKET_H
#define _USBIP_WEBSOCK_POCO_USBWS_WEBSOCKET_H

#include "Poco/Net/WebSocket.h"
#include "Poco/Util/Application.h"
#include "Poco/Timer.h"
#include "Poco/Mutex.h"

namespace Usbip { namespace WebSock { namespace Poco {

using namespace ::Poco;
using namespace ::Poco::Net;
using namespace ::Poco::Util;

class USBWSWebSocket :
	public ::Poco::Net::WebSocket,
	public ::Poco::Timer
{
public:
	USBWSWebSocket(HTTPServerRequest& req, HTTPServerResponse& rsp,
			int pingPong);
	USBWSWebSocket(HTTPClientSession& cs,
			HTTPRequest& req, HTTPResponse& rsp, int pingPong);
	static ssize_t send(void *arg, void *buf, size_t len);
	static ssize_t recv(void *arg, void *buf, size_t len, int wait_all);
	static void shutdown(void *arg);
	int getSockfd(void) { return sockfd(); };
	void setApp(Application* app) { fApp = app; };
	Application* getApp(void) { return fApp; };
private:
	Application* fApp;
	bool fPingPongStarted;
	bool fFirstTimeout;
	Mutex fSendLock;

	ssize_t send(void *buf, size_t len);
	ssize_t recv(void *buf, size_t len, int wait_all);
	void shutdown(void);
	void startPingPong(void);
	void restartPingPong(void);
	void onTimer(Timer& timer);
	void sendPing(void);
	void sendPong(void);
	void sendPingPong(int op);
};

}}}

#endif /* !_USBIP_WEBSOCK_POCO_USBWS_WEBSOCKET_H */

