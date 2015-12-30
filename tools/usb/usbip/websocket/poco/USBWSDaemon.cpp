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

#include "USBWSDaemon.h"
#include "USBWSRequestHandlerFactory.h"
#include "USBWSUtil.h"
#include "Poco/Net/HTTPServer.h"
#include "Poco/Net/HTTPServerParams.h"
#include "Poco/Net/ServerSocket.h"
#include "Poco/Net/SecureServerSocket.h"
#include "Poco/Util/HelpFormatter.h"
#include <iostream>

extern "C" {
#include "usbip_common.h"
#include "usbipd.h"
}

using namespace ::Poco;
using namespace ::Poco::Net;
using namespace ::Poco::Util;
using namespace ::Usbip::WebSock::Poco;

const std::string USBWSDaemon::kDefaultPath("/usbip");
const std::string USBWSDaemon::kDefaultKey("cert/server.key");
const std::string USBWSDaemon::kDefaultCert("cert/server.crt");
const int USBWSDaemon::kMajorVersion(0);
const int USBWSDaemon::kMinorVersion(0);
const int USBWSDaemon::kRevision(1);

USBWSDaemon::USBWSDaemon() :
	fDebug(false),
	fTcpPort(0),
	fPath(kDefaultPath),
	fPingPong(60),
	fSSL(false),
	fKey(kDefaultKey),
	fCert(kDefaultCert),
	fRootCert(),
	fVerificationStr(),
	fVerification(Context::VERIFY_NONE),
	fHelp(false),
	fVersion(false),
	fSocket(0)
{
}

USBWSDaemon::~USBWSDaemon()
{
}

void USBWSDaemon::initialize(Application& self)
{
	ServerApplication::initialize(self);
}

void USBWSDaemon::uninitialize(void)
{
	ServerApplication::uninitialize();
}

void USBWSDaemon::defineOptions(OptionSet& options)
{
	ServerApplication::defineOptions(options);
	options.addOption(Option("debug", "d", "Print debug information."));
	options.addOption(Option("tcp-port", "t", "Port number to listen."
		).argument("port-number"));
	options.addOption(Option("path", "p",
		"WebSocket path to serve USBIP/IP. Default is "
		+ kDefaultPath + "."
		).argument("path"));
	options.addOption(Option("interval", "i",
		"Noncommunication time period to send ping-pong in seconds."
		" Default is 60. 0 denotes not to use ping-pong."
		).argument("interval-sec"));
	options.addOption(Option("ssl", "s", "Enable SSL."));
	options.addOption(Option("key", "k",
		"Private key file. Default is " + kDefaultKey + "."
		).argument("cert-file"));
	options.addOption(Option("cert", "c",
		"Certificate file. Default is " + kDefaultCert + "."
		).argument("key-file"));
	options.addOption(Option("root-cert", "r",
		"Certificate file of root CA."
		).argument("root-cert-file"));
	options.addOption(Option("verification", "V",
		"Certificate verification mode -"
		" none(default), relaxed, strict or once."
		).argument("verification-mode"));
	options.addOption(Option("help", "h", "Print this help."));
	options.addOption(Option("version", "v", "Show version."));
}

void USBWSDaemon::handleOption(const std::string& name, const std::string & value)
{
	ServerApplication::handleOption(name, value);

	if (name == "debug") {
		fDebug = true;
		usbip_use_debug = 1;
	} else if (name == "tcp-port") {
		fTcpPort = USBWSUtil::s2i(value);
	} else if (name == "path") {
		fPath = value;
	} else if (name == "interval") {
		fPingPong = USBWSUtil::s2i(value);
	} else if (name == "ssl") {
		fSSL = true;
	} else if (name == "key") {
		fKey = value;
	} else if (name == "cert") {
		fCert = value;
	} else if (name == "root-cert") {
		fRootCert = value;
	} else if (name == "verification") {
		fVerificationStr = value;
	} else if (name == "help") {
		fHelp = true;
	} else if (name == "version") {
		fVersion = true;
	}
}

void USBWSDaemon::help()
{
	HelpFormatter helpFormatter(options());
	helpFormatter.setCommand(commandName());
	helpFormatter.format(std::cout);
}

void USBWSDaemon::version()
{
	std::cout << kMajorVersion << "." << kMinorVersion << "." << kRevision << std::endl;
}

int USBWSDaemon::main(const std::vector<std::string>& args)
{
	usbip_use_stderr = 1;
	if (fHelp) {
		help();
		return Application::EXIT_OK;
	}
	if (fVersion) {
		version();
		return Application::EXIT_OK;
	}
	if (fTcpPort == 0) {
		if (fSSL) {
			fTcpPort = 443;
		} else {
			fTcpPort = 80;
		}
	}
	if (fVerificationStr.size() > 0) {
		if (fVerificationStr == "none") {
			fVerification = Context::VERIFY_NONE;
		} else if (fVerificationStr == "relaxed") {
			fVerification = Context::VERIFY_RELAXED;
		} else if (fVerificationStr == "strict") {
			fVerification = Context::VERIFY_STRICT;
		} else if (fVerificationStr == "once") {
			fVerification = Context::VERIFY_ONCE;
		} else {
			std::cerr << "Unsupported verification mode." << std::endl;
			return Application::EXIT_USAGE;
		}
	}
	if (usbip_driver_open()) {
		logger().error("Fail to open vhci driver.");
		return Application::EXIT_IOERR;
	}
	if (openSocket()) {
		logger().error("Fail to open socket.");
		return Application::EXIT_IOERR;
	}
	HTTPServer svr(
		new USBWSRequestHandlerFactory(fPath, fPingPong, logger()),
		*fSocket, new HTTPServerParams());
	svr.start();
	dbg("Waiting at %d:%s %s",
		fTcpPort, fPath.c_str(), fSSL? "with SSL" : "");
	waitForTerminationRequest();
	logger().information("Stopping server.");
	usbip_break_connections();
	svr.stop();
	closeSocket();
	usbip_driver_close();
	return Application::EXIT_OK;
}

POCO_SERVER_MAIN(USBWSDaemon)

int USBWSDaemon::openSocket()
{
	if (fSSL) {
		Context *ctx = new Context(Context::SERVER_USE, fKey, fCert, fRootCert, fVerification);
		// TODO: manipulate other params.
		fSocket = new SecureServerSocket(fTcpPort, 64, ctx);
	} else {
		fSocket = new ServerSocket(fTcpPort, 64);
	}
	return 0;
}

void USBWSDaemon::closeSocket()
{
	if (fSocket) {
		delete fSocket;
	}
	fSocket = 0;
}

