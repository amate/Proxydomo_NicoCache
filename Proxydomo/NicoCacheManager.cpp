/**
*	@file	NicoCacheManager.cpp
*/

#include "stdafx.h"
#include "NicoCacheManager.h"
#include "proximodo\util.h"

#define CR	'\r'
#define LF	'\n'
#define CRLF "\r\n"


namespace {


	std::unique_ptr<CSocket> ConnectWebsite(const std::string& contactHost)
	{
		// The host string is composed of host and port
		std::string name = contactHost;
		std::string port;
		size_t colon = name.find(':');
		if (colon != std::string::npos) {    // (this should always happen)
			port = name.substr(colon + 1);
			name = name.substr(0, colon);
		}
		if (port.empty())
			port = "80";


		// Check the host (Hostname() asks the DNS)
		IPv4Address host;
		if (name.empty() || host.SetService(port) == false || host.SetHostName(name) == false) {
			throw std::runtime_error("502 Bad Gateway");
			// The host address is invalid (or unknown by DNS)
			// so we won't try a connection.
			//_FakeResponse("502 Bad Gateway", "./html/error.html");
			//fakeResponse("502 Bad Gateway", "./html/error.html", true,
			//             CSettings::ref().getMessage("502_BAD_GATEWAY"),
			//             name);
			return;
		}

		// Connect
		auto psockWebsite = std::make_unique<CSocket>();
		do {
			if (psockWebsite->Connect(host))
				break;
		} while (host.SetNextHost());

		if (psockWebsite->IsConnected() == false) {
			// Connection failed, warn the browser
			throw std::runtime_error("502 Bad Gateway");
			//_FakeResponse("503 Service Unavailable", "./html/error.html");
			//fakeResponse("503 Service Unavailable", "./html/error.html", true,
			//             CSettings::ref().getMessage("503_UNAVAILABLE"),
			//             contactHost);
			return;
		}
		return psockWebsite;
	}

}	// namespace


///////////////////////////////////////////////////
// CNicoCacheManager

const std::string kGetFlvURL = "http://flapi.nicovideo.jp/api/getflv/";

bool CNicoCacheManager::IsGetFlvURL(const CUrl& url)
{
	if (url.getUrl().compare(0, kGetFlvURL.length(), kGetFlvURL) != 0)
		return false;

	return true;
}

void CNicoCacheManager::TrapGetFlv(CFilterOwner& filterOwner, std::unique_ptr<CSocket>& sockBrowser)
{
	std::unique_ptr<CSocket> sockBrowser2 = std::move(sockBrowser);
	sockBrowser.reset(new CSocket);

	if (CUtil::noCaseContains("Keep-Alive", CFilterOwner::GetHeader(filterOwner.outHeadersFiltered, "Proxy-Connection"))) {
		CFilterOwner::RemoveHeader(filterOwner.outHeadersFiltered, "Proxy-Connection");
		CFilterOwner::SetHeader(filterOwner.outHeadersFiltered, "Connection", "Keep-Alive");
	}
	auto sockWebsite = ConnectWebsite(filterOwner.contactHost);

	std::string sendOutBuf = "GET " + filterOwner.url.getAfterHost() + " HTTP/1.1" CRLF;
	for (auto& pair : filterOwner.outHeadersFiltered)
		sendOutBuf += pair.first + ": " + pair.second + CRLF;
	sendOutBuf += CRLF;

	if (sockWebsite->Write(sendOutBuf.c_str(), sendOutBuf.length()) == false)
		throw std::runtime_error("write error");
}








