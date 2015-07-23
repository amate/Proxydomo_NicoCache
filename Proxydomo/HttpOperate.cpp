/**
*
*/

#include "stdafx.h"
#include "HttpOperate.h"
#include <boost\format.hpp>
#include "Logger.h"
#include "FilterOwner.h"
#include "RequestManager.h"
#include "CodeConvert.h"
#include "proximodo\util.h"

using namespace CodeConvert;

#define CR	'\r'
#define LF	'\n'
#define CRLF "\r\n"

namespace {
	
	enum { kReadBuffSize = 64 * 1024 };	// 64kbyte

	bool	GetHeaders(std::string& buf, HeadPairList& headers)
	{
		for (;;) {
			// Look for end of line
			size_t pos, len;
			if (CUtil::endOfLine(buf, 0, pos, len) == false)
				return false;	// 改行がないので帰る

								// Check if we reached the empty line
			if (pos == 0) {
				buf.erase(0, len);
				return true;	// 終了
			}

			// Find the header end
			while (pos + len >= buf.size()
				|| buf[pos + len] == ' '
				|| buf[pos + len] == '\t'
				)
			{
				if (CUtil::endOfLine(buf, pos + len, pos, len) == false)
					return false;
			}

			// Record header
			size_t colon = buf.find(':');
			if (colon != std::string::npos) {
				std::wstring name = UTF16fromUTF8(buf.substr(0, colon));
				std::wstring value = UTF16fromUTF8(buf.substr(colon + 1, pos - colon - 1));
				CUtil::trim(value);
				headers.emplace_back(std::move(name), std::move(value));
			}
			//log += buf.substr(0, pos + len);
			buf.erase(0, pos + len);
		}
	}

	// contactHost へ接続する 接続に失敗した場合例外を出す
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
		}
		return psockWebsite;
	}



}	// namespace


bool ReadSocketBuffer(CSocket* sock, std::string& buffer)
{
	bool bDataReceived = false;
	char readBuffer[kReadBuffSize];
	while (sock->IsDataAvailable() && sock->Read(readBuffer, kReadBuffSize)) {
		int count = sock->GetLastReadCount();
		if (count == 0)
			break;
		buffer.append(readBuffer, count);
		bDataReceived = true;
	}
	return bDataReceived;
}

void WriteSocketBuffer(CSocket* sock, const char* buffer, int length)
{
	try {
		sock->Write(buffer, length);
	}
	catch (std::exception& e) {
		ERROR_LOG << L"WriteSocketBuffer : " << e.what();
		throw;
	}
}


std::unique_ptr<CSocket>	SendRequest(const CUrl& url, HttpVerb verb, HeadPairList& outHeaders)
{
	// サイトへ接続
	auto sockWebsite = ConnectWebsite(UTF8fromUTF16(url.getHostPort()));

	// 送信ヘッダを編集
	if (CUtil::noCaseContains(L"Keep-Alive", CFilterOwner::GetHeader(outHeaders, L"Proxy-Connection"))) {
		CFilterOwner::RemoveHeader(outHeaders, L"Proxy-Connection");
		CFilterOwner::SetHeader(outHeaders, L"Connection", L"Keep-Alive");
	}

	LPCSTR strVerb = nullptr;
	switch (verb) {
	case HttpVerb::kHttpGet:
		strVerb = "GET";
		break;

	case HttpVerb::kHttpPost:
		strVerb = "POST";
		break;

	default:
		ATLASSERT(FALSE);
		throw std::runtime_error("invalid verb");
	}
	std::string sendOutBuf = (boost::format("%1% %2% HTTP/1.1" CRLF) % strVerb % UTF8fromUTF16(url.getAfterHost())).str();
	for (auto& pair : outHeaders)
		sendOutBuf += UTF8fromUTF16(pair.first) + ": " + UTF8fromUTF16(pair.second) + CRLF;
	sendOutBuf += CRLF;

	// リクエストを送信
	WriteSocketBuffer(sockWebsite.get(), sendOutBuf.c_str(), static_cast<int>(sendOutBuf.length()));

	return sockWebsite;
}


void GetResponseHeader(CFilterOwner& filterOwner, CSocket* sockWebsite, std::string& buffer)
{
	STEP inStep = STEP::STEP_FIRSTLINE;
	for (;;) {
		bool bRead = ReadSocketBuffer(sockWebsite, buffer);

		switch (inStep) {
		case STEP::STEP_FIRSTLINE:
		{
			// Do we have the full first line yet?
			if (buffer.size() < 4)
				break;
			// ゴミデータが詰まってるので終わる
			if (::strncmp(buffer.c_str(), "HTTP", 4) != 0) {
				buffer.clear();
				throw std::runtime_error("gomi data");
			}

			size_t pos, len;
			if (CUtil::endOfLine(buffer, 0, pos, len) == false)
				break;		// まだ改行まで読み込んでないので帰る

							// Parse it
			size_t p1 = buffer.find_first_of(" ");
			size_t p2 = buffer.find_first_of(" ", p1 + 1);
			filterOwner.responseLine.ver = buffer.substr(0, p1);
			filterOwner.responseLine.code = buffer.substr(p1 + 1, p2 - p1 - 1);
			filterOwner.responseLine.msg = buffer.substr(p2 + 1, pos - p2 - 1);
			filterOwner.responseCode = buffer.substr(p1 + 1, pos - p1 - 1);

			// Remove it from receive-in buffer.
			// we don't place it immediately on send-in buffer,
			// as we may be willing to fake it (cf $REDIR)
			//m_logResponse = m_recvInBuf.substr(0, pos + len);
			buffer.erase(0, pos + len);

			// Next step will be to read headers
			inStep = STEP::STEP_HEADERS;
			continue;
		}
		break;

		case STEP::STEP_HEADERS:
		{
			if (GetHeaders(buffer, filterOwner.inHeaders) == false)
				continue;

			inStep = STEP::STEP_DECODE;
			return;	// ヘッダ取得終了
		}
		break;

		}	// switch

		if (bRead == false) {
			::Sleep(10);
		}

		if (sockWebsite->IsConnected() == false)
			throw std::runtime_error("connection close");
	}	// for
}


std::string GetResponseBody(CFilterOwner& filterOwner, CSocket* sockWebsite, std::string& buffer)
{
	int64_t inSize = 0;
	std::string contentLength = UTF8fromUTF16(filterOwner.GetInHeader(L"Content-Length"));
	if (contentLength.size() > 0) {
		inSize = boost::lexical_cast<int64_t>(contentLength);
	} else {
		ATLASSERT(FALSE);	// チャンク形式には未対応
		throw std::runtime_error("no content-length");
	}

	for (; sockWebsite->IsConnected();) {
		if (inSize <= buffer.size())
			break;

		if (ReadSocketBuffer(sockWebsite, buffer) == false)
			::Sleep(10);
	}
	ATLASSERT(buffer.length() == inSize);

	std::wstring contentEncoding = filterOwner.GetInHeader(L"Content-Encoding");
	if (CUtil::noCaseContains(L"gzip", contentEncoding)) {
		filterOwner.RemoveInHeader(L"Content-Encoding");
		CZlibBuffer decompressor;
		decompressor.reset(false, true);

		decompressor.feed(buffer);
		decompressor.read(buffer);

		filterOwner.SetInHeader(L"Content-Length", std::to_wstring(buffer.length()));
	}
	return std::move(buffer);
}


std::string GetHTTPPage(CFilterOwner& filterOwner)
{
	// リクエストを送信
	auto sockWebsite = SendRequest(filterOwner.url, HttpVerb::kHttpGet, filterOwner.outHeaders);

	// HTTP Header を受信する
	std::string buffer;
	GetResponseHeader(filterOwner, sockWebsite.get(), buffer);
	std::string body = GetResponseBody(filterOwner, sockWebsite.get(), buffer);
	sockWebsite->Close();
	return body;
}




std::string SendResponse(CFilterOwner& filterOwner, CSocket* sockBrowser, const std::string& body)
{
	ATLASSERT(filterOwner.responseLine.code.length() && filterOwner.responseLine.msg.length());

	std::string sendInBuf = "HTTP/1.1 " + filterOwner.responseLine.code + " " + filterOwner.responseLine.msg + CRLF;
	for (auto& pair : filterOwner.inHeaders)
		sendInBuf += UTF8fromUTF16(pair.first) + ": " + UTF8fromUTF16(pair.second) + CRLF;
	sendInBuf += CRLF;
	sendInBuf += body;

	std::wstring contentLength = filterOwner.GetInHeader(L"Content-Length");
	if (body.length() > 0) {
		ATLASSERT(contentLength.length() > 0);
		ATLASSERT(body.length() == std::stoll(contentLength));
	}

	// レスポンスを送信
	WriteSocketBuffer(sockBrowser, sendInBuf.c_str(), sendInBuf.length());
	return sendInBuf;
}

void	SendHTTPOK_Response(CSocket* sockBrowser, HeadPairList& inHeaders, const std::string& body)
{
	std::string sendInBuf = "HTTP/1.1 200 OK" CRLF;
	for (auto& pair : inHeaders)
		sendInBuf += UTF8fromUTF16(pair.first) + ": " + UTF8fromUTF16(pair.second) + CRLF;
	sendInBuf += CRLF;

	std::wstring contentLength = CFilterOwner::GetHeader(inHeaders, L"Content-Length");
	if (body.length() > 0) {
		ATLASSERT(contentLength.length() > 0);
		ATLASSERT(body.length() == std::stoll(contentLength));
	}

	// レスポンスを送信
	WriteSocketBuffer(sockBrowser, sendInBuf.c_str(), sendInBuf.length());
	WriteSocketBuffer(sockBrowser, body.c_str(), body.length());	
}

























