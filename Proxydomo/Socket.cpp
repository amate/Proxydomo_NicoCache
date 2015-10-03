/**
*	@file	Socket.cpp
*	@brief	�\�P�b�g�N���X
*/
/**
	this file is part of Proxydomo
	Copyright (C) amate 2013-

	This program is free software; you can redistribute it and/or
	modify it under the terms of the GNU General Public License
	as published by the Free Software Foundation; either
	version 2 of the License, or (at your option) any later version.

	This program is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with this program; if not, write to the Free Software
	Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
*/
#include "stdafx.h"
#include "Socket.h"
#include <chrono>
#pragma comment(lib, "ws2_32.lib")

#include "DebugWindow.h"
#include "Logger.h"
#include "Settings.h"


////////////////////////////////////////////////////////////////////
// IPv4Address

IPv4Address::IPv4Address() : current_addrinfo(nullptr)
{
	::SecureZeroMemory(&addr, sizeof(addr));
	addr.sin_family	= AF_INET;
#ifdef _DEBUG
	port = 0;
#endif
}

IPv4Address& IPv4Address::operator = (sockaddr_in sockaddr)
{
	addr = sockaddr;
#ifdef _DEBUG
	ip = ::inet_ntoa(addr.sin_addr);
	port = ::ntohs(sockaddr.sin_port);
#endif
	return *this;
}

void IPv4Address::SetPortNumber(uint16_t port)
{
	addr.sin_port = htons(port);
#ifdef _DEBUG
	this->port = port;
#endif
}


bool IPv4Address::SetService(const std::string& protocol)
{
	if (protocol == "http") {
		SetPortNumber(80);
		return true;
	} else if (protocol == "https") {
		SetPortNumber(443);
		return true;
	}
	try {
		SetPortNumber(boost::lexical_cast<uint16_t>(protocol));
		return true;
	} catch (...) {
		return false;
	}
}

bool IPv4Address::SetHostName(const std::string& IPorHost)
{
	auto ipret = ::inet_addr(IPorHost.c_str());
	if (ipret != INADDR_NONE) {
		addr.sin_addr.S_un.S_addr = ipret;
#ifdef _DEBUG
		ip = ::inet_ntoa(addr.sin_addr);
#endif
		return true;
	}
	std::string service = boost::lexical_cast<std::string>(GetPortNumber());
	ATLASSERT( !service.empty() );
	addrinfo* result = nullptr;
	addrinfo hinsts = {};
	hinsts.ai_socktype	= SOCK_STREAM;
	hinsts.ai_family	= AF_INET;
	hinsts.ai_protocol	= IPPROTO_TCP;
	for (int tryCount = 0;; ++tryCount) {
		int ret = ::getaddrinfo(IPorHost.c_str(), service.c_str(), &hinsts, &result);
		if (ret == 0) {
			break;
		} else if (ret == EAI_AGAIN) {
			if (tryCount >= 5) {
				WARN_LOG << L"getaddrinfo retry failed: " << IPorHost;
				return false;
			}
			::Sleep(50);
		} else {
			std::wstring strerror = gai_strerror(ret);
			return false;
		}
	}

	addrinfoList.reset(result, [](addrinfo* p) { ::freeaddrinfo(p); });
	current_addrinfo	= addrinfoList.get();

	sockaddr_in sockaddr = *(sockaddr_in*)addrinfoList->ai_addr;
	operator =(sockaddr);

#ifdef _DEBUG
		ip = ::inet_ntoa(addr.sin_addr);
#endif
	return true;
}	

bool IPv4Address::SetNextHost()
{
	if (current_addrinfo == nullptr)
		return false;
	current_addrinfo = current_addrinfo->ai_next;
	if (current_addrinfo) {
		sockaddr_in sockaddr = *(sockaddr_in*)current_addrinfo->ai_addr;
		operator =(sockaddr);
		return true;
	}	
	return false;
}

////////////////////////////////////////////////////////////////
// CSocket

CSocket::CSocket() : m_sock(0), m_nLastReadCount(0), m_nLastWriteCount(0)
{
	m_writeStop = false;
}


bool CSocket::Init()
{
	WSADATA wsaData;
	WORD wVersionRequested = MAKEWORD( 2, 2 );
	int nRet = ::WSAStartup( wVersionRequested, &wsaData );
	ATLASSERT( nRet == 0 );
	if (nRet == 0)
		return true;
	else
		return false;
}

void CSocket::Term()
{
	int nRet = ::WSACleanup();
	ATLASSERT( nRet == 0 );
}


IPv4Address CSocket::GetFromAddress() const
{
	ATLASSERT( m_sock );
	return m_addrFrom;
}

// �|�[�g�ƃ\�P�b�g���֘A�t����
void	CSocket::Bind(uint16_t port)
{
	m_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (m_sock ==  INVALID_SOCKET)
		throw SocketException("Can`t open socket");

	SetBlocking(false);
	_SetReuse(true);

	sockaddr_in localAddr = { 0 };
	localAddr.sin_family = AF_INET;
	localAddr.sin_port = htons(port);
	if (CSettings::s_privateConnection) {
		localAddr.sin_addr.s_addr = ::inet_addr("127.0.0.1");
	} else {
		localAddr.sin_addr.s_addr = INADDR_ANY;
	}
	if( ::bind(m_sock, (sockaddr *)&localAddr, sizeof(localAddr)) == SOCKET_ERROR)
		throw SocketException("Can`t bind socket");

	if (::listen(m_sock, SOMAXCONN) == SOCKET_ERROR)
		throw SocketException("Can`t listen",WSAGetLastError());
}

// �|�[�g����̉�����҂�
std::unique_ptr<CSocket>	CSocket::Accept()
{
	fd_set readfds;
	FD_ZERO(&readfds);
	FD_SET(m_sock, &readfds);

	enum { kTimeout = 500 * 1000 };
	timeval timeout = {};
	timeout.tv_usec = kTimeout;
	int ret = ::select(static_cast<int>(m_sock) + 1, &readfds, nullptr, nullptr, &timeout);
	if (ret == SOCKET_ERROR)
		throw SocketException("select failed");

	if (FD_ISSET(m_sock, &readfds) == false)
		return nullptr;

	int fromSize = sizeof(sockaddr_in);
	sockaddr_in from;
	SOCKET conSock = ::accept(m_sock, (sockaddr *)&from, &fromSize);
	if (conSock ==  INVALID_SOCKET)
		return nullptr;

	// �ڑ���������
    CSocket*	pSocket = new CSocket;
	pSocket->m_sock = conSock;
	pSocket->m_addrFrom = from;
	//pSocket->_setBlocking(false);
	//pSocket->_setBufSize(65535);

	return std::unique_ptr<CSocket>(std::move(pSocket));;
}

bool	CSocket::Connect(IPv4Address addr)
{
	ATLASSERT( m_sock == 0 );
	m_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (m_sock == INVALID_SOCKET) {
		m_sock = 0;
		throw SocketException("Can`t open socket");
	}
	int ret = ::connect(m_sock, addr, sizeof(sockaddr_in));
	if (ret == SOCKET_ERROR) {
		::closesocket(m_sock);
		m_sock = 0;
		return false;
	} else {
		return true;
	}
}

void	CSocket::Close()
{
	if (m_sock) {
		::shutdown(m_sock, SD_SEND);

		//setReadTimeout(2000);
		auto starttime = std::chrono::steady_clock::now();
		char c[1024];
		try {
			while (IsDataAvailable()) {
				int len = ::recv(m_sock, c, sizeof(c), 0);
				if (len == 0 || len == SOCKET_ERROR)
					break;
				if (std::chrono::steady_clock::now() - starttime > std::chrono::seconds(5))
					break;
			}
		} catch (SocketException& e) {
			ATLTRACE( e.what() );
			ERROR_LOG << L"CSocket::Close : " << e.what();
		}
		::shutdown(m_sock, SD_RECEIVE);
		if (::closesocket(m_sock) == SOCKET_ERROR) {
			m_sock = 0;
			//ATLASSERT( FALSE );
			throw SocketException("closesocket failed");
			//LOG_ERROR("closesocket() error");
		}
		m_sock = 0;
	}
}


bool	CSocket::IsDataAvailable()
{
	if (m_sock == 0)
		return false;

	fd_set readfds;
	FD_ZERO(&readfds);
	FD_SET(m_sock, &readfds);
	TIMEVAL nonwait = {};
	int ret = ::select(0, &readfds, nullptr, nullptr, &nonwait);
	if (ret == SOCKET_ERROR)
		throw SocketException("IsDataAvailable failed");

	if (FD_ISSET(m_sock, &readfds))
		return true;
	else
		return false;

#if 0
	std::shared_ptr<std::remove_pointer<HANDLE>::type> hEvent(::WSACreateEvent(), [](HANDLE h) {
		::WSACloseEvent(h);
	});

	if (hEvent.get() == WSA_INVALID_EVENT)
		throw SocketException("WSACreateEvent failed");

	if (::WSAEventSelect(m_sock, hEvent.get(), FD_READ) == SOCKET_ERROR)
		throw SocketException("WSAEventSelect failed");

	HANDLE h[1] = { hEvent.get() };
	DWORD dwRet = ::WSAWaitForMultipleEvents(1, h, FALSE, 0, FALSE);
	::WSAEventSelect(m_sock, hEvent.get(), 0);
	DWORD dwTemp = 0;
	::ioctlsocket(m_sock, FIONBIO, &dwTemp);
	if (dwRet == WSA_WAIT_EVENT_0)
		return true;
	else
		return false;
#endif
}

bool	 CSocket::Read(char* buffer, int length)
{
	if (m_sock == 0)
		return false;

	ATLASSERT(length > 0);
	m_nLastReadCount = 0;
	int ret = ::recv(m_sock, buffer, length, 0);
	if (ret == 0) {
		Close();
		return true;

	} else {
		int wsaError = ::WSAGetLastError();
		if (ret == SOCKET_ERROR && wsaError == WSAEWOULDBLOCK) {
			return false;	// pending
		}
		if (ret < 0) {
			Close();
			return false;

		} else {
			m_nLastReadCount = ret;
			return true;

		}
	}
}


bool	CSocket::Write(const char* buffer, int length)
{
	if (m_sock == 0)
		return false;

	m_nLastWriteCount = 0;
	ATLASSERT( length > 0 );
	int ret = 0;
	int trycount = 0;
	enum { kMaxRetryCount = 200 };
	for (;;) {
		ret = ::send(m_sock, buffer, length, 0);
		int wsaError = ::WSAGetLastError();
		if (ret == SOCKET_ERROR && wsaError == WSAEWOULDBLOCK && m_writeStop == false) {
			++trycount;
			if (kMaxRetryCount < trycount) {
				ERROR_LOG << L"CSocket::Write : max retry";
				break;
			}

			::Sleep(10);
		} else {
			break;
		}
	}

	if (ret != length) {
		Close();
		return false;

	} else {
		m_nLastWriteCount = ret;
		return true;
	}
}


// --------------------------------------------------
void CSocket::SetBlocking(bool yes)
{
	unsigned long op = yes ? 0 : 1;
	if (ioctlsocket(m_sock, FIONBIO, &op) == SOCKET_ERROR)
		throw SocketException("Can`t set blocking");
}

// --------------------------------------------------
void CSocket::_SetReuse(bool yes)
{
	unsigned long op = yes ? 1 : 0;
	if (setsockopt(m_sock, SOL_SOCKET, SO_REUSEADDR, (char *)&op, sizeof(op)) == SOCKET_ERROR) 
		throw SocketException("Unable to set REUSE");
}

















