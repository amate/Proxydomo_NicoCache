/**
*	@file	NicoCacheManager.cpp
*/

#include "stdafx.h"
#include "NicoCacheManager.h"
#include <regex>
#include <fstream>
#include <chrono>
#include <boost\format.hpp>
#include <Mmsystem.h>
#pragma comment(lib, "Winmm.lib")
#include "proximodo\util.h"
#include "RequestManager.h"
#include "Logger.h"
#include "Misc.h"
#include "CodeConvert.h"
#include "MediaInfo.h"

using namespace CodeConvert;
using namespace std::chrono;

#define CR	'\r'
#define LF	'\n'
#define CRLF "\r\n"


namespace {

	enum { kReadBuffSize = 64 * 1024 };

	std::wstring GetCacheFolderPath()
	{
		return std::wstring(Misc::GetExeDirectory() + L"nico_cache\\");
	}

	std::wstring Get_smNumberFilePath(const std::string& smNumber)
	{
		std::wstring searchPath = GetCacheFolderPath() + UTF16fromUTF8(smNumber) + L"*";
		WIN32_FIND_DATA fd = {};
		HANDLE hFind = ::FindFirstFile(searchPath.c_str(), &fd);
		if (hFind == INVALID_HANDLE_VALUE) {
			return L"";
		} else {
			FindClose(hFind);

			std::wstring filePath = GetCacheFolderPath() + fd.cFileName;
			return filePath;
		}
	}

	std::wstring GetThumbCacheFolderPath()
	{
		return std::wstring(Misc::GetExeDirectory() + L"nico_cache\\thumb_cache\\");
	}

	std::wstring GetThumbCachePath(const std::wstring& number)
	{
		std::wstring searchPath = GetThumbCacheFolderPath() + number + L".jpg*";
		WIN32_FIND_DATA fd = {};
		HANDLE hFind = ::FindFirstFile(searchPath.c_str(), &fd);
		if (hFind == INVALID_HANDLE_VALUE) {
			return L"";
		} else {
			FindClose(hFind);

			std::wstring filePath = GetThumbCacheFolderPath() + fd.cFileName;
			return filePath;
		}
	}

	// 拡張子はつけない
	std::wstring CreateCacheFilePath(const std::string& smNumber, bool bLowReqeust)
	{
		std::wstring cachePath = GetCacheFolderPath() + UTF16fromUTF8(smNumber);
		if (bLowReqeust) {
			cachePath += L"low";
		}
		std::wstring title = CNicoCacheManager::Get_smNumberTitle(smNumber);
		ATLASSERT(title.size());
		cachePath += L"_" + title;
		return cachePath;
	}


	/// ファイル作成時の無効な文字を置換する
	void MtlValidateFileName(std::wstring& strName, LPCTSTR replaceChar = _T("-"))
	{
		strName = CUtil::replaceAll(strName, L"\\", replaceChar);
		strName = CUtil::replaceAll(strName, L"/", L"／");
		strName = CUtil::replaceAll(strName, L":", L"：");
		strName = CUtil::replaceAll(strName, L"*", L"＊");
		strName = CUtil::replaceAll(strName, L"?", L"？");
		strName = CUtil::replaceAll(strName, L"\"", L"'");
		strName = CUtil::replaceAll(strName, L"<", L"＜");
		strName = CUtil::replaceAll(strName, L">", L"＞");
		strName = CUtil::replaceAll(strName, L"|", L"｜");

		strName = CUtil::replaceAll(strName, L"&amp;", L"&");
	}

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
		}
		return psockWebsite;
	}

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

	void WriteSocketBuffer(std::unique_ptr<CSocket>& sock, const char* buffer, int length)
	{
		try {
			sock->Write(buffer, length);
		}
		catch (std::exception& e) {
			ERROR_LOG << L"WriteSocketBuffer : " << e.what();
		}
	}

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

	void GetHTTPHeader(CFilterOwner& filterOwner, CSocket* sock, std::string& buffer)
	{
		STEP inStep = STEP::STEP_FIRSTLINE;
		for (;;) {
			bool bRead = ReadSocketBuffer(sock, buffer);

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

			if (sock->IsConnected() == false)
				throw std::runtime_error("connection close");
		}	// for
	}

	std::string GetHTTPBody(CFilterOwner& filterOwner, CSocket* sock, std::string& buffer)
	{
		int64_t inSize = 0;
		std::string contentLength = UTF8fromUTF16(filterOwner.GetInHeader(L"Content-Length"));
		if (contentLength.size() > 0) {
			inSize = boost::lexical_cast<int64_t>(contentLength);
		} else {
			throw std::runtime_error("no content-length");
		}

		for (; sock->IsConnected();) {
			if (inSize <= buffer.size())
				break;

			if (ReadSocketBuffer(sock, buffer) == false)
				::Sleep(10);
		}
		return std::move(buffer);
	}

	// ファイル内容を読み込み
	std::string LoadFile(const std::wstring& filePath)
	{
		std::ifstream fscache(filePath, std::ios::in | std::ios::binary);
		if (!fscache)
			throw std::runtime_error("file open failed : " + std::string(CW2A(filePath.c_str())));

		std::string data;
		fscache.seekg(0, std::ios::end);
		streamoff fileSize = fscache.tellg();
		fscache.seekg(0, std::ios::beg);
		fscache.clear();
		data.resize(static_cast<size_t>(fileSize));
		fscache.read(const_cast<char*>(data.data()), static_cast<size_t>(fileSize));
		fscache.close();

		return data;
	}



	class CCacheFileManager
	{
	public:
		CCacheFileManager(const std::string& smNumber, bool bLowRequest) :
			m_smNumber(smNumber), m_bLowRequest(bLowRequest), m_cacheComplete(false), m_lowCacheComplete(false)
		{}

		bool	SearchCache()
		{
			std::wstring cachePath = Get_smNumberFilePath(m_smNumber + "_");
			if (cachePath.empty()) {
				cachePath = Get_smNumberFilePath("#" + m_smNumber + "_");
			}
			std::wstring lowCachePath = Get_smNumberFilePath(m_smNumber + "low_");

			auto funcIsFileComplete = [](const std::wstring& path) -> bool {
				if (path.empty())
					return false;

				CString ext = Misc::GetFileExt(path.c_str());
				ext.MakeLower();
				if (ext == L"incomplete") {
					return false;
				} else {
					return true;
				}
			};

			LPCWSTR kIncompleteCacheTail = L".mp4.incomplete";
			LPCWSTR kCompleteCacheTail = L".mp4";

			m_cacheComplete = funcIsFileComplete(cachePath);
			if (cachePath.length()) {
				if (m_cacheComplete) {
					m_completeCachePath = cachePath;
				} else {
					m_incompleteCachePath = cachePath;
					m_completeCachePath = CreateCacheFilePath(m_smNumber, false) + kCompleteCacheTail;
				}
			} else {
				m_incompleteCachePath = CreateCacheFilePath(m_smNumber, false) + kIncompleteCacheTail;
				m_completeCachePath = CreateCacheFilePath(m_smNumber, false) + kCompleteCacheTail;
			}

			m_lowCacheComplete = funcIsFileComplete(lowCachePath);
			if (lowCachePath.length()) {
				if (m_lowCacheComplete) {
					m_completeLowCachePath = lowCachePath;
				} else {
					m_incompleteLowCachePath = lowCachePath;
					m_completeLowCachePath = CreateCacheFilePath(m_smNumber, true) + kCompleteCacheTail;
				}
			} else {
				m_incompleteLowCachePath = CreateCacheFilePath(m_smNumber, true) + kIncompleteCacheTail;
				m_completeLowCachePath = CreateCacheFilePath(m_smNumber, true) + kCompleteCacheTail;
			}

			m_cachePath = cachePath;
			m_lowCachePath = lowCachePath;
			return IsCacheComplete();
		}

		bool	IsCacheComplete() const {
			return (m_cachePath.length() > 0) ||
				(m_lowCachePath.length() > 0 && m_bLowRequest);
		}

		const std::wstring& LoadCachePath() const
		{
			if (m_cacheComplete || m_cachePath.length()) {
				return m_cachePath;
			} else {
				return m_lowCachePath;
			}
		}

		bool	IsLoadCacheComplete() const
		{
			if (m_cacheComplete) {
				return true;
			} else {
				if (m_cachePath.length()) {
					return false;
				} else {
					return m_lowCacheComplete;
				}
			}
		}

		const std::wstring& IncompleteCachePath() const
		{
			if (m_bLowRequest) {
				return m_incompleteLowCachePath;
			} else {
				return m_incompleteCachePath;
			}
		}

		const std::wstring& CompleteCachePath() const
		{
			if (m_bLowRequest) {
				return m_completeLowCachePath;
			} else {
				return m_completeCachePath;
			}
		}

		void	MoveFileIncompleteToComplete()
		{
			BOOL bRet = ::MoveFile(IncompleteCachePath().c_str(), CompleteCachePath().c_str());
			if (bRet == 0) {
				ERROR_LOG << L"MoveFile failed : src : " << IncompleteCachePath() << L" dest : " << CompleteCachePath();
			}
			if (m_bLowRequest == false && m_lowCachePath.length()) {
				::DeleteFile(m_lowCachePath.c_str());
				INFO_LOG << L"古いキャッシュを削除しました。 : " << m_lowCachePath;
			}
		}

	private:
		std::string	m_smNumber;
		bool		m_bLowRequest;

		std::wstring m_cachePath;
		bool		 m_cacheComplete;
		std::wstring m_incompleteCachePath;
		std::wstring m_completeCachePath;

		std::wstring m_lowCachePath;
		bool		 m_lowCacheComplete;
		std::wstring m_incompleteLowCachePath;
		std::wstring m_completeLowCachePath;
	};


	bool	VideoConveter(const std::string& smNumber, const std::wstring& filePath)
	{
		enum { kMaxReFrames = 5, kBadweightb = 2 };
		auto videoInfo = GetVideoInfo(filePath);
		if (videoInfo == nullptr) {
			ATLASSERT(FALSE);
			return false;
		}

		CSize resolution;
		resolution.cx = videoInfo->width;
		resolution.cy = videoInfo->height;

		//int bit = std::atoi(result.str(3).c_str());
		auto atPos = videoInfo->formatProfile.find(L'@');
		ATLASSERT(atPos != std::wstring::npos);

		std::wstring profile = videoInfo->formatProfile.substr(0, atPos);
		std::wstring level = videoInfo->formatProfile.substr(atPos + 2);

		INFO_LOG << L"VideoConveter : " << smNumber 
				<< L" formatProfile : " << videoInfo->formatProfile << L" ref_frames : " << videoInfo->ref_frames;

		if ((profile == L"High" && level[0] == '5') || 
			(kMaxReFrames <= videoInfo->ref_frames && videoInfo->weightb == kBadweightb) ) 
		{
			// 960x640 iPhone4S 解像度
			CSize destResolution = resolution;
			if (destResolution.cx > 960) {
				int destY = (960 * destResolution.cy) / destResolution.cx;
				if ((destY % 4) != 0) {	// 4の倍数になるよう調節する
					destY -= destY % 4;
				}
				destResolution.cx = 960;
				destResolution.cy = destY;
			}

			std::thread([smNumber, filePath, destResolution]() {
				CString qsvEncPath = Misc::GetExeDirectory() + _T("QSVEncC\\x64\\QSVEncC64.exe");
				std::wstring title = CNicoCacheManager::Get_smNumberTitle(smNumber);
				std::wstring outPath = GetCacheFolderPath() + UTF16fromUTF8(smNumber) + L"enc_" + title + L".mp4";
				CString commandLine = 
					(boost::wformat(L"--avqsv --copy-audio --cqp 30 --quality 1 --output-res %1%x%2% -i \"%3%\" -o \"%4%\"") 
										% destResolution.cx % destResolution.cy % filePath % outPath).str().c_str();
				STARTUPINFO startUpInfo = { sizeof(STARTUPINFO) };
				PROCESS_INFORMATION processInfo = {};

				INFO_LOG << L"VideoConveter EncStart : " << smNumber << L" commandLine : " << (LPCWSTR)commandLine;

				BOOL bRet = ::CreateProcess(qsvEncPath, (LPWSTR)(LPCWSTR)commandLine, 
											nullptr, nullptr, FALSE, CREATE_NEW_CONSOLE, nullptr, nullptr, &startUpInfo, &processInfo);
				ATLASSERT(bRet);
				::WaitForSingleObject(processInfo.hProcess, INFINITE);
				::CloseHandle(processInfo.hThread);
				::CloseHandle(processInfo.hProcess);

				std::wstring backupPath = GetCacheFolderPath() + UTF16fromUTF8(smNumber) + L"org_" + title + L".mp4";
				bRet = ::MoveFile(filePath.c_str(), backupPath.c_str());
				if (bRet) {
					bRet = ::MoveFile(outPath.c_str(), filePath.c_str());
					if (bRet) {
						// 完了！
						INFO_LOG << L"VideoConveter EncFinish! : " << smNumber;

						CString SEpath = Misc::GetExeDirectory() + _T("宝箱出現.wav");
						ATLVERIFY(::PlaySound(SEpath, NULL, SND_FILENAME | SND_SYNC));

					} else {
						ERROR_LOG << L"MoveFile failed : src : " << outPath << L" dest : " << filePath;
					}					
				} else {
					ERROR_LOG << L"MoveFile failed : src : " << filePath << L" dest : " << backupPath;
				}


			}).detach();

			return true;
		} else {
			return false;
		}
	}

}	// namespace


///////////////////////////////////////////////////
// CNicoMovieCacheManager

void	CNicoMovieCacheManager::StartThread(
	const std::string& smNumber,
	CFilterOwner& filterOwner, std::unique_ptr<CSocket>&& sockBrowser)
{
	INFO_LOG << L"CNicoMovieCacheManager::StartThread : " << smNumber;

	auto manager = new CNicoMovieCacheManager;
	manager->m_active = true;
	manager->m_smNumber = smNumber;

	manager->_CreateTransactionData(smNumber);

	manager->NewBrowserConnection(filterOwner, std::move(sockBrowser));

	CCritSecLock lock2(CNicoCacheManager::s_cssmNumberCacheManager);
	CNicoCacheManager::s_mapsmNumberCacheManager.emplace_front(
								std::make_pair(smNumber, std::unique_ptr<CNicoMovieCacheManager>(std::move(manager))));
	manager->m_thisThread = std::thread([manager]() {

		try {
			manager->Manage();
		}
		catch (std::exception& e) {
			ERROR_LOG << L"CNicoMovieCacheManager::StartThread : " << e.what();
		}

		manager->m_thisThread.detach();

		CCritSecLock lock2(CNicoCacheManager::s_cssmNumberCacheManager);
		auto& list = CNicoCacheManager::s_mapsmNumberCacheManager.get<CNicoCacheManager::hash>();
		auto it = list.find(manager->smNumber());
		ATLASSERT(it != list.end());
		list.erase(it);
	});
}

void	CNicoMovieCacheManager::StartThread(const std::string& smNumber, const NicoRequestData& nicoRequestData)
{
	INFO_LOG << L"CNicoMovieCacheManager::StartThread : " << smNumber;

	auto manager = new CNicoMovieCacheManager;
	manager->m_active = true;
	manager->m_smNumber = smNumber;

	manager->_CreateTransactionData(smNumber);

	manager->m_optNicoRequestData = nicoRequestData;

	CCritSecLock lock2(CNicoCacheManager::s_cssmNumberCacheManager);
	CNicoCacheManager::s_mapsmNumberCacheManager.emplace_front(
		std::make_pair(smNumber, std::unique_ptr<CNicoMovieCacheManager>(std::move(manager))));
	manager->m_thisThread = std::thread([manager]() {

		try {
			manager->Manage();
		}
		catch (std::exception& e) {
			ERROR_LOG << L"CNicoMovieCacheManager::StartThread : " << e.what();
		}

		manager->m_thisThread.detach();

		CCritSecLock lock2(CNicoCacheManager::s_cssmNumberCacheManager);
		auto& list = CNicoCacheManager::s_mapsmNumberCacheManager.get<CNicoCacheManager::hash>();
		auto it = list.find(manager->smNumber());
		ATLASSERT(it != list.end());
		list.erase(it);
	});
}

void	CNicoMovieCacheManager::_CreateTransactionData(const std::string& smNumber)
{
	bool bLowRequest = false;
	std::string movieURL = CNicoCacheManager::Get_smNumberMovieURL(m_smNumber);
	CString temp = movieURL.c_str();
	if (temp.Right(3) == _T("low")) {
		bLowRequest = true;
	}

	std::wstring transName;
	CCacheFileManager cacheFileManager(smNumber, bLowRequest);
	if (cacheFileManager.SearchCache() && cacheFileManager.IsLoadCacheComplete()) {
		transName = cacheFileManager.LoadCachePath();
	} else {
		transName = CreateCacheFilePath(smNumber, bLowRequest);
	}
	auto slashPos = transName.rfind(L'\\');
	ATLASSERT(slashPos != std::wstring::npos);
	transName = transName.substr(slashPos + 1);
	m_transactionData = CNicoCacheManager::CreateTransactionData(transName);
}

void	CNicoMovieCacheManager::NewBrowserConnection(CFilterOwner& filterOwner, std::unique_ptr<CSocket>&& sockBrowser)
{
	CCritSecLock lock(m_csBrowserRangeRequestList);
	m_browserRangeRequestList.emplace_front();
	auto itThis = m_browserRangeRequestList.begin();
	itThis->filterOwner = filterOwner;
	itThis->sockBrowser = std::move(sockBrowser);
	itThis->itThis = itThis;

	m_transactionData->AddBrowserTransaction(static_cast<void*>(&*itThis));
}

void	CNicoMovieCacheManager::_InitRangeSetting(BrowserRangeRequest& browserRangeRequest)
{
	std::wstring range = CFilterOwner::GetHeader(browserRangeRequest.filterOwner.outHeaders, L"Range");
	INFO_LOG << L"_InitRangeSetting : " << browserRangeRequest.GetID();

	if (range.empty()) {	// Range リクエストではない
		browserRangeRequest.browserRangeBegin = 0;
		browserRangeRequest.browserRangeEnd = m_movieSize - 1;
		browserRangeRequest.rangeBufferPos = 0;

		browserRangeRequest.filterOwner.RemoveInHeader(L"Accept-Ranges");
		browserRangeRequest.filterOwner.RemoveInHeader(L"Content-Range");
		browserRangeRequest.filterOwner.SetInHeader(L"Content-Length", std::to_wstring(m_movieSize));

	} else {
		std::wregex rx(L"bytes=(\\d+)-(\\d+)");
		std::wsmatch result;
		if (std::regex_search(range, result, rx) == false) {
			// ブラウザから
			std::wregex rx2(L"bytes=(\\d+)-");
			std::wsmatch result2;
			if (std::regex_search(range, result2, rx2) == false) {
				throw std::runtime_error("ragex range not found");
			} else {
				browserRangeRequest.browserRangeBegin = boost::lexical_cast<int64_t>(result2.str(1));
				browserRangeRequest.browserRangeEnd = m_movieSize - 1;
			}

		} else {
			browserRangeRequest.browserRangeBegin = boost::lexical_cast<int64_t>(result.str(1));
			browserRangeRequest.browserRangeEnd = boost::lexical_cast<int64_t>(result.str(2));
		}

		browserRangeRequest.browserRangeLength = browserRangeRequest.browserRangeEnd - browserRangeRequest.browserRangeBegin + 1;
		browserRangeRequest.rangeBufferPos = browserRangeRequest.browserRangeBegin;


		browserRangeRequest.filterOwner.SetInHeader(L"Accept-Ranges", L"bytes");
		browserRangeRequest.filterOwner.SetInHeader(L"Content-Range",
			(boost::wformat(L"bytes %1%-%2%/%3%") % browserRangeRequest.browserRangeBegin % browserRangeRequest.browserRangeEnd % m_movieSize).str());
		browserRangeRequest.filterOwner.SetInHeader(L"Content-Length", std::to_wstring(browserRangeRequest.browserRangeLength));
	}

	browserRangeRequest.filterOwner.SetInHeader(L"Content-Type", L"video/mp4");

	browserRangeRequest.filterOwner.SetInHeader(L"Connection", L"close");

	auto browserData = m_transactionData->GetBrowserTransactionData(static_cast<void*>(&browserRangeRequest));
	browserData->browserRangeBegin = browserRangeRequest.browserRangeBegin;
	browserData->browserRangeEnd = browserRangeRequest.browserRangeEnd;
	browserData->rangeBufferPos = browserRangeRequest.rangeBufferPos;
}

void	CNicoMovieCacheManager::_SendResponseHeader(BrowserRangeRequest& browserRangeRequest)
{
	std::string sendInBuf;
	std::wstring range = CFilterOwner::GetHeader(browserRangeRequest.filterOwner.outHeaders, L"Range");
	if (range.empty()) {
		sendInBuf = "HTTP/1.1 200 OK" CRLF;
	} else {
		sendInBuf = "HTTP/1.1 206 Partial Content" CRLF;
	}
	std::string name;
	for (auto& pair : browserRangeRequest.filterOwner.inHeaders)
		sendInBuf += UTF8fromUTF16(pair.first) + ": " + UTF8fromUTF16(pair.second) + CRLF;
	sendInBuf += CRLF;

	// HTTP Header を送信
	WriteSocketBuffer(browserRangeRequest.sockBrowser, sendInBuf.c_str(), sendInBuf.length());
		//throw std::runtime_error("sockBrowser write error");

	browserRangeRequest.bSendResponseHeader = true;
}

void	CNicoMovieCacheManager::SwitchToInvalid()
{
	m_active = false;
}

void	CNicoMovieCacheManager::ForceStop()
{
	m_thisThread.detach();
}


void CNicoMovieCacheManager::Manage()
{
	// 既存のキャッシュを調べる
	m_movieSize = 0;
	bool bIncomplete = false;
	NicoRequestData nicoRequestData;
	std::ofstream fs;

	bool bLowRequest = false;
	std::string movieURL = CNicoCacheManager::Get_smNumberMovieURL(m_smNumber);
	CString temp = movieURL.c_str();
	if (temp.Right(3) == _T("low")) {
		bLowRequest = true;
	}

	CCacheFileManager cacheFileManager(m_smNumber, bLowRequest);
	if (cacheFileManager.SearchCache()) {

		INFO_LOG << m_smNumber << L" cacheFile found : " << cacheFileManager.LoadCachePath();

		// ファイル内容を読み込み
		m_movieCacheBuffer = LoadFile(cacheFileManager.LoadCachePath());

		// キャッシュが完全化どうか
		if (cacheFileManager.IsLoadCacheComplete() == false) {
			INFO_LOG << m_smNumber << L" incomplete cache file FileSize : " << m_movieCacheBuffer.size();

			bIncomplete = true;

		} else {
			INFO_LOG << m_smNumber << L" cache file is complete!!";

			// キャッシュは完全だった
			m_movieSize = m_movieCacheBuffer.size();
			m_transactionData->fileSize = m_movieSize;
			m_transactionData->lastDLPos = m_movieSize;
		}
	} 

	// キャッシュがないのでサイトからDLする
	if (m_movieSize == 0) {

		// 既にキャッシュがある
		if (bIncomplete) {
			fs.open(cacheFileManager.IncompleteCachePath(), std::ios::out | std::ios::binary | std::ios::app);

		} else {
			INFO_LOG << m_smNumber << L" create new cache file : " << cacheFileManager.IncompleteCachePath();
			// 新規キャッシュを始める
			fs.open(cacheFileManager.IncompleteCachePath(), std::ios::out | std::ios::binary);
		}
		if (!fs)
			throw std::runtime_error("file open failed : " + std::string(CW2A(cacheFileManager.IncompleteCachePath().c_str())));

		CUrl url;
		std::wstring contactHost;
		HeadPairList outHeaders;
		if (m_optNicoRequestData) {	// DLのみ
			url = m_optNicoRequestData->url;
			contactHost = m_optNicoRequestData->url.getHostPort();
			outHeaders = m_optNicoRequestData->outHeaders;
		} else {
			url = m_browserRangeRequestList.front().filterOwner.url;
			contactHost = m_browserRangeRequestList.front().filterOwner.contactHost;
			outHeaders = m_browserRangeRequestList.front().filterOwner.outHeaders;
		}

		// サイトへ接続
		m_sockWebsite = ConnectWebsite(UTF8fromUTF16(contactHost));

		// 送信ヘッダを編集
		auto outHeadersFiltered = outHeaders;

		if (CUtil::noCaseContains(L"Keep-Alive", CFilterOwner::GetHeader(outHeadersFiltered, L"Proxy-Connection"))) {
			CFilterOwner::RemoveHeader(outHeadersFiltered, L"Proxy-Connection");
			CFilterOwner::SetHeader(outHeadersFiltered, L"Connection", L"Keep-Alive");
		}

		if (bIncomplete) {
			// Range ヘッダを変更して途中からデータを取得する
			std::wstring range = (boost::wformat(L"bytes=%1%-") % m_movieCacheBuffer.size()).str();
			CFilterOwner::SetHeader(outHeadersFiltered, L"Range", range);
		} else {
			// Range ヘッダを削ってファイル全体を受信する設定にする
			CFilterOwner::RemoveHeader(outHeadersFiltered, L"Range");
		}

		CFilterOwner::RemoveHeader(outHeadersFiltered, L"If-Modified-Since");
		CFilterOwner::RemoveHeader(outHeadersFiltered, L"If-None-Match");

		std::string sendOutBuf = "GET " + UTF8fromUTF16(url.getAfterHost()) + " HTTP/1.1" CRLF;
		for (auto& pair : outHeadersFiltered)
			sendOutBuf += UTF8fromUTF16(pair.first) + ": " + UTF8fromUTF16(pair.second) + CRLF;
		sendOutBuf += CRLF;

		// GET リクエストを送信
		if (m_sockWebsite->Write(sendOutBuf.c_str(), sendOutBuf.length()) == false)
			throw std::runtime_error("sockWebsite write error");

		// HTTP Header を受信する
		std::string buffer;
		CFilterOwner filterOwner;
		GetHTTPHeader(filterOwner, m_sockWebsite.get(), buffer);

		// ファイルサイズ取得
		if (bIncomplete) {
			std::wstring contentRange = filterOwner.GetInHeader(L"Content-Range");
			std::wregex rx(L"bytes \\d+-\\d+/(\\d+)");
			std::wsmatch result;
			if (std::regex_search(contentRange, result, rx) == false)
				throw std::runtime_error("contentRange not found");

			m_movieSize = boost::lexical_cast<int64_t>(result.str(1));

		} else {
			std::string contentLength = UTF8fromUTF16(filterOwner.GetInHeader(L"Content-Length"));
			if (contentLength.size() > 0) {
				m_movieSize = boost::lexical_cast<int64_t>(contentLength);
			} else {
				throw std::runtime_error("no content-length");
			}
			ATLASSERT(m_movieCacheBuffer.empty());
		}
		INFO_LOG << m_smNumber << L" ファイルサイズ : " << m_movieSize;
		m_transactionData->fileSize = m_movieSize;

		fs.write(buffer.c_str(), buffer.size());
		m_movieCacheBuffer += buffer;
		m_lastMovieSize = m_movieCacheBuffer.size();
		m_transactionData->lastDLPos = m_lastMovieSize;

		if (m_optNicoRequestData) {
			nicoRequestData = m_optNicoRequestData.get();
		} else {
			nicoRequestData.url = url;
			nicoRequestData.outHeaders = outHeaders;

			_InitRangeSetting(m_browserRangeRequestList.front());
			_SendResponseHeader(m_browserRangeRequestList.front());
		}
	}

	bool debugSizeCheck = false;
	steady_clock::time_point lastTime;
	for (;;) {
		{
			CCritSecLock lock(m_csBrowserRangeRequestList);
			for (auto& browserRangeRequest : m_browserRangeRequestList) {
				if (browserRangeRequest.bSendResponseHeader)
					continue;

				_InitRangeSetting(browserRangeRequest);
				_SendResponseHeader(browserRangeRequest);

				lastTime = steady_clock::time_point();
				INFO_LOG << browserRangeRequest.GetID() << L" browser request";
			}
		}

		std::list<std::list<BrowserRangeRequest>::iterator> suicideList;

		bool bRead = false;
		if (m_sockWebsite && m_sockWebsite->IsConnected()) {
			bRead = ReadSocketBuffer(m_sockWebsite.get(), m_movieCacheBuffer);
			if (bRead) {
				size_t bufferSize = m_movieCacheBuffer.size();
				if (fs) {
					size_t writeSize = bufferSize - m_lastMovieSize;
					fs.write(m_movieCacheBuffer.c_str() + m_lastMovieSize, writeSize);
					m_lastMovieSize = bufferSize;
				}
				m_transactionData->lastDLPos = bufferSize;

				if (bufferSize == m_movieSize) {
					INFO_LOG << m_smNumber << L" のダウンロードを完了しました！";
					m_sockWebsite->Close();
					m_sockWebsite.reset();

					if (fs) {
						fs.close();

						cacheFileManager.MoveFileIncompleteToComplete();

						if (bLowRequest == false) {
							if (VideoConveter(m_smNumber, cacheFileManager.CompleteCachePath())) {
								CCritSecLock lock(m_transactionData->csData);
								m_transactionData->detailText = _T("エンコードを開始します。");
								++m_transactionData->lastDLPos;
							}
						}
					}
					CNicoCacheManager::ConsumeDLQue();
				}
			}
		} else {
			if (debugSizeCheck == false) {
				if (m_movieCacheBuffer.size() != m_movieSize) {
					ERROR_LOG << L"サイトとの接続が切れたが、ファイルをすべてDLできませんでした...";
				}
			}
			debugSizeCheck = true;
		}
		size_t bufferSize = m_movieCacheBuffer.size();
	
		{
			CCritSecLock lock(m_csBrowserRangeRequestList);
			for (auto& browserRangeRequest : m_browserRangeRequestList) {
				if (browserRangeRequest.bSendResponseHeader == false)
					continue;

				if (browserRangeRequest.sockBrowser->IsConnected() == false) {
					INFO_LOG << browserRangeRequest.GetID() << L" browser disconnection";
					suicideList.emplace_back(browserRangeRequest.itThis);

				} else {
					if (browserRangeRequest.rangeBufferPos < bufferSize) {
						int64_t restRangeSize = browserRangeRequest.browserRangeEnd - browserRangeRequest.rangeBufferPos + 1;
						int64_t sendSize = bufferSize - browserRangeRequest.rangeBufferPos;
						if (restRangeSize < sendSize)
							sendSize = restRangeSize;

						//INFO_LOG << browserRangeRequest.GetID() << L" RangeBegin : " << browserRangeRequest.browserRangeBegin << L" RangeEnd : " << browserRangeRequest.browserRangeEnd << L" RangeBufferPos : " << browserRangeRequest.rangeBufferPos << L" sendSize : " << sendSize;

						WriteSocketBuffer(browserRangeRequest.sockBrowser, 
											m_movieCacheBuffer.c_str() + browserRangeRequest.rangeBufferPos, sendSize);
						browserRangeRequest.rangeBufferPos += sendSize;

						auto browserData = m_transactionData->GetBrowserTransactionData(static_cast<void*>(&browserRangeRequest));
						browserData->rangeBufferPos = browserRangeRequest.rangeBufferPos;

						if (browserRangeRequest.rangeBufferPos > browserRangeRequest.browserRangeEnd) {	// 終わり
							INFO_LOG << browserRangeRequest.GetID() << L" browser close";
							browserRangeRequest.sockBrowser->Close();
							suicideList.emplace_back(browserRangeRequest.itThis);
						}
					} else {
						//INFO_LOG << L"RangeBufferPos : " << m_rangeBufferPos << L" bufferSize : " << bufferSize;
					}
				}
			}
		}

		{
			CCritSecLock lock(m_csBrowserRangeRequestList);	
			for (auto it : suicideList) {
				m_transactionData->RemoveBrowserTransaction(static_cast<void*>(&*it));
				m_browserRangeRequestList.erase(it);
			}
			suicideList.clear();

			// キャッシュコンプリート済み かつ ブラウザからのリクエストが一定時間なければ終了させる
			if (m_sockWebsite == nullptr && m_browserRangeRequestList.empty()) {
				if (lastTime == steady_clock::time_point()) {
					lastTime = steady_clock::now();
				} else {
					if ((steady_clock::now() - lastTime) > seconds(30)) {
						SwitchToInvalid();
					}
				}
			}
		}

		if (bRead == false) {
			if (m_active == false) {
				INFO_LOG << m_smNumber << L" active false break";
				if (m_sockWebsite && m_sockWebsite->IsConnected()) {
					m_sockWebsite->Close();
				}
				{
					CCritSecLock lock(m_csBrowserRangeRequestList);
					for (auto& browserRangeRequest : m_browserRangeRequestList) {
						if (browserRangeRequest.sockBrowser->IsConnected()) {
							browserRangeRequest.sockBrowser->Close();
						}
					}
				}

				if (m_movieCacheBuffer.size() != m_movieSize) {
					CNicoCacheManager::AddDLQue(m_smNumber, nicoRequestData);
				}
				break;
			}

			::Sleep(50);
		}
	}

	CNicoCacheManager::DestroyTransactionData(m_transactionData);

	INFO_LOG << m_smNumber << L" Manage finish";
}


///////////////////////////////////////////////////
// CNicoCacheManager

CCriticalSection	CNicoCacheManager::s_csMovieChunkList;
CNicoCacheManager::MovieChunkContainer	CNicoCacheManager::s_movieChunkList;

CCriticalSection	CNicoCacheManager::s_cssmNumberCacheManager;
CNicoCacheManager::ManagerContainer	CNicoCacheManager::s_mapsmNumberCacheManager;

CCriticalSection	CNicoCacheManager::s_cssmNumberDLQue;
CNicoCacheManager::QueContainer	CNicoCacheManager::s_smNumberDLQue;

std::unique_ptr<CNicoConnectionFrame>	CNicoCacheManager::s_nicoConnectionFrame;


void CNicoCacheManager::CreateNicoConnectionFrame()
{
	s_nicoConnectionFrame.reset(new CNicoConnectionFrame);
	s_nicoConnectionFrame->Create(NULL);
}

std::shared_ptr<TransactionData>	CNicoCacheManager::CreateTransactionData(const std::wstring& name)
{
	return s_nicoConnectionFrame->CreateTransactionData(name);
}

void	CNicoCacheManager::DestroyTransactionData(std::shared_ptr<TransactionData> transData)
{
	s_nicoConnectionFrame->DestroyTransactionData(transData);
}


void CNicoCacheManager::CloseAllConnection()
{
	s_nicoConnectionFrame->DestroyWindow();

	{
		CCritSecLock lock(s_cssmNumberCacheManager);
		for (auto& pair : s_mapsmNumberCacheManager) {			
			pair.second->SwitchToInvalid();
		}
	}

	int count = 0;
	enum { kMaxRetryCount = 50 };
	while (s_mapsmNumberCacheManager.size()) {
		::Sleep(100);
		++count;
		if (count > kMaxRetryCount)
			break;
	}
	if (s_mapsmNumberCacheManager.size() > 0) {
		ERROR_LOG << L"CNicoCacheManager::CloseAllConnection timeout";		
		CCritSecLock lock(s_cssmNumberCacheManager);
		for (auto& pair : s_mapsmNumberCacheManager) {
			pair.second->ForceStop();
		}
	}
}

const std::wstring kGetFlvURL = L"http://flapi.nicovideo.jp/api/getflv";

bool CNicoCacheManager::IsGetFlvURL(const CUrl& url)
{
	if (url.getUrl().compare(0, kGetFlvURL.length(), kGetFlvURL) != 0)
		return false;

	return true;
}

void CNicoCacheManager::TrapGetFlv(CFilterOwner& filterOwner, CSocket* sockBrowser)
{
	try {
		std::string query = UTF8fromUTF16(filterOwner.url.getQuery());
		std::string smNumber = query.substr(3);	// ?v=smXXX

		// サイトへ接続
		auto sockWebsite = ConnectWebsite(UTF8fromUTF16(filterOwner.contactHost));

		if (CUtil::noCaseContains(L"Keep-Alive", CFilterOwner::GetHeader(filterOwner.outHeaders, L"Proxy-Connection"))) {
			CFilterOwner::RemoveHeader(filterOwner.outHeaders, L"Proxy-Connection");
			CFilterOwner::SetHeader(filterOwner.outHeaders, L"Connection", L"Keep-Alive");
		}

		std::string sendOutBuf = "GET " + UTF8fromUTF16(filterOwner.url.getAfterHost()) + " HTTP/1.1" CRLF;
		for (auto& pair : filterOwner.outHeaders)
			sendOutBuf += UTF8fromUTF16(pair.first) + ": " + UTF8fromUTF16(pair.second) + CRLF;
		sendOutBuf += CRLF;

		// GET リクエストを送信
		if (sockWebsite->Write(sendOutBuf.c_str(), sendOutBuf.length()) == false)
			throw std::runtime_error("sockWebsite write error");

		// HTTP Header を受信する
		std::string buffer;
		GetHTTPHeader(filterOwner, sockWebsite.get(), buffer);

		std::string sendInBuf = "HTTP/1.1 " + filterOwner.responseLine.code + " " + filterOwner.responseLine.msg + CRLF;
		std::string name;
		for (auto& pair : filterOwner.inHeaders)
			sendInBuf += UTF8fromUTF16(pair.first) + ": " + UTF8fromUTF16(pair.second) + CRLF;
		sendInBuf += CRLF;

		// HTTP Header を送信
		if (sockBrowser->Write(sendInBuf.c_str(), sendInBuf.length()) == false)
			throw std::runtime_error("sockBrowser write error");

		// Body を受信
		std::string body = GetHTTPBody(filterOwner, sockWebsite.get(), buffer);
		sockWebsite->Close();
		if (filterOwner.responseLine.code != "200")
			throw std::runtime_error("responseCode not 200 : " + filterOwner.responseCode);

		// Body を送信
		if (sockBrowser->Write(body.c_str(), body.length()) == false)
			throw std::runtime_error("sockBrowser write error");

		// movieURL を取得
		std::string escBody = CUtil::UESC(body);
		std::regex rx("url=([^&]+)");
		std::smatch result;
		if (std::regex_search(escBody, result, rx) == false)
			throw std::runtime_error("url not found");

		std::string movieURL = result.str(1);
		INFO_LOG << L"smNumber : " << smNumber << L" movieURL : " << movieURL;

		Associate_movieURL_smNumber(movieURL, smNumber);
	}
	catch (std::exception& e) {
		WARN_LOG << L"CNicoCacheManager::TrapGetFlv : " << e.what();
	}
}

void CNicoCacheManager::Associate_movieURL_smNumber(const std::string& movieURL, const std::string& smNumber)
{
	CCritSecLock lock(s_csMovieChunkList);
	auto& hashList = s_movieChunkList.get<smnum>();
	auto it = hashList.find(smNumber);
	if (it != hashList.end()) {
		ATLVERIFY(hashList.modify(it, [movieURL](MovieChunk& chunk) {
			chunk.movieURL = movieURL;
		}));
	} else {
		s_movieChunkList.emplace(smNumber, movieURL);
	}
	INFO_LOG << L"Associate_movieURL_smNumber : " << smNumber << L" movieURL : " << movieURL;
}


bool CNicoCacheManager::IsMovieURL(const CUrl& url)
{
	CCritSecLock lock(s_csMovieChunkList);
	auto& hashList = s_movieChunkList.get<mvurl>();
	auto it = hashList.find(UTF8fromUTF16(url.getUrl()));
	if (it != hashList.end()) {
		return true;
	} else {
		ATLASSERT(url.getUrl().find(L"nicovideo.jp/smile") == std::wstring::npos);
		return false;
	}
}

void CNicoCacheManager::ManageMovieCache(CFilterOwner& filterOwner, std::unique_ptr<CSocket>& sockBrowser)
{
	CCritSecLock lock(s_csMovieChunkList);
	auto& hashList = s_movieChunkList.get<mvurl>();
	auto it = hashList.find(UTF8fromUTF16(filterOwner.url.getUrl()));
	ATLASSERT(it != hashList.end());

	std::string smNumber = it->smNumber;
	lock.Unlock();

	CCritSecLock lock2(s_cssmNumberCacheManager);
	auto& list = s_mapsmNumberCacheManager.get<hash>();
	auto itManager = list.find(smNumber);
	if (itManager != list.end()) {
		if (itManager->second->IsValid() == false) {
			ERROR_LOG << smNumber << L" 既に存在するマネージャーが無効になっています";
			return;	// リトライ処理を入れるべき？
		}
		itManager->second->NewBrowserConnection(filterOwner, std::move(sockBrowser));
	} else {
		// 同時DL数を超すようなら一番古いマネージャーを止めてから追加する
		int activeManageCount = 0;
		for (auto& pair : s_mapsmNumberCacheManager) {
			if (pair.second->IsValid()) {
				++activeManageCount;
			}
		}
		INFO_LOG << smNumber << L" activeManageCount : " << activeManageCount;
		if (kMaxParallelDLCount <= activeManageCount) {
			auto& orderList = s_mapsmNumberCacheManager.get<seq>();
			for (auto it = orderList.rbegin(); it != orderList.rend(); ++it) {
				if (it->second->IsValid()) {
					it->second->SwitchToInvalid();
					INFO_LOG << it->second->smNumber() << L" を無効にしました";
					break;
				}
			}
		}
		lock2.Unlock();

		{	// キューに入っていれば削除する
			CCritSecLock lock3(s_cssmNumberDLQue);
			auto& hashList = s_smNumberDLQue.get<hash>();
			auto it = hashList.find(smNumber);
			if (it != hashList.end()) {
				hashList.erase(it);
			}
		}

		CNicoMovieCacheManager::StartThread(smNumber, filterOwner, std::move(sockBrowser));
	}
	sockBrowser.reset(new CSocket);
}

void CNicoCacheManager::Associate_smNumberTitle(const std::string& smNumber, const std::wstring& title)
{
	CCritSecLock lock(s_csMovieChunkList);
	std::wstring movieTitle = title;
	MtlValidateFileName(movieTitle);

	auto& hashList = s_movieChunkList.get<smnum>();
	auto it = hashList.find(smNumber);
	if (it != hashList.end()) {
		ATLVERIFY(hashList.modify(it, [movieTitle](MovieChunk& chunk) {
			chunk.title = movieTitle;
		}));
	} else {
		hashList.emplace(smNumber, smNumber, movieTitle);
	}
	INFO_LOG << L"Associate_smNumberTitle : " << smNumber << L" title : " << movieTitle;
}


std::wstring CNicoCacheManager::Get_smNumberTitle(const std::string& smNumber)
{
	CCritSecLock lock(s_csMovieChunkList);
	auto& hashList = s_movieChunkList.get<smnum>();
	auto it = hashList.find(smNumber);
	ATLASSERT(it != hashList.end());
	if (it != hashList.end()) {
		std::wstring title = it->title;
		return title;
	} else {
		return L"";
	}
}

std::string CNicoCacheManager::Get_smNumberMovieURL(const std::string& smNumber)
{
	CCritSecLock lock(s_csMovieChunkList);
	auto& hashList = s_movieChunkList.get<smnum>();
	auto it = hashList.find(smNumber);
	ATLASSERT(it != hashList.end());
	if (it != hashList.end()) {
		std::string movieURL = it->movieURL;
		return movieURL;
	} else {
		return "";
	}

}

void CNicoCacheManager::AddDLQue(const std::string& smNumber, const NicoRequestData& nicoRequestData)
{
	CCritSecLock lock(s_cssmNumberDLQue);
	s_smNumberDLQue.emplace_back(std::make_pair(smNumber, nicoRequestData));

	INFO_LOG << L"CNicoCacheManager::AddDLQue : " << smNumber;
}

void CNicoCacheManager::ConsumeDLQue()
{
	CCritSecLock lock(s_cssmNumberDLQue);
	if (s_smNumberDLQue.empty())
		return;

	auto& seqList = s_smNumberDLQue.get<seq>();
	auto requestData = seqList.front();
	seqList.erase(seqList.begin());
	lock.Unlock();

	INFO_LOG << L"CNicoCacheManager::ConsumeDLQue : " << requestData.first;
	CNicoMovieCacheManager::StartThread(requestData.first, requestData.second);
}


bool CNicoCacheManager::IsThumbURL(const CUrl& url)
{
	std::wregex rx(L"http://[^.]+\\.smilevideo\\.jp/smile\\?i=(.*)");
	std::wsmatch result;
	if (std::regex_match(url.getUrl(), result, rx)) {
		return true;
	} else {
		return false;
	}
}


CCriticalSection	g_csSaveThumb;

void CNicoCacheManager::ManageThumbCache(CFilterOwner& filterOwner, std::unique_ptr<CSocket>& sockBrowser)
{
	std::wregex rx(L"http://[^.]+\\.smilevideo\\.jp/smile\\?i=(.*)");
	std::wsmatch result;
	if (std::regex_match(filterOwner.url.getUrl(), result, rx) == false) {
		ATLASSERT(FALSE);
		return;
	}

	std::wstring number = result.str(1);
	std::wstring thumbCachePath = GetThumbCachePath(number);

	auto funcSendThumb = [&sockBrowser](const std::string& fileContent) {
		std::string sendInBuf;
		sendInBuf = "HTTP/1.1 200 OK" CRLF;

		HeadPairList inHeader;
		CFilterOwner::SetHeader(inHeader, L"Content-Type", L"image/jpeg");
		CFilterOwner::SetHeader(inHeader, L"Content-Length", std::to_wstring(fileContent.size()));
		CFilterOwner::SetHeader(inHeader, L"Connection", L"keep-alive");

		for (auto& pair : inHeader)
			sendInBuf += UTF8fromUTF16(pair.first) + ": " + UTF8fromUTF16(pair.second) + CRLF;
		sendInBuf += CRLF;

		// HTTP Header を送信
		WriteSocketBuffer(sockBrowser, sendInBuf.c_str(), sendInBuf.length());

		// Body を送信
		WriteSocketBuffer(sockBrowser, fileContent.c_str(), fileContent.length());
	};

	bool bIncomplete = false;
	if (thumbCachePath.length() > 0) {
		CString ext = Misc::GetFileExt(thumbCachePath.c_str());
		ext.MakeLower();
		if (ext != L"incomplete") {
			std::string fileContent = LoadFile(thumbCachePath);
			funcSendThumb(fileContent);

			INFO_LOG << L"Send ThumbCache : " << number;
			return;

		} else {
			INFO_LOG << L"Send ThumbCache incomplete found : " << number;
			bIncomplete = true;
		}

	}

	// サイトへ接続	
	auto sockWebsite = ConnectWebsite(UTF8fromUTF16(filterOwner.url.getHostPort()));

	// 送信ヘッダを編集
	auto outHeadersFiltered = filterOwner.outHeaders;

	if (CUtil::noCaseContains(L"Keep-Alive", CFilterOwner::GetHeader(outHeadersFiltered, L"Proxy-Connection"))) {
		CFilterOwner::RemoveHeader(outHeadersFiltered, L"Proxy-Connection");
		CFilterOwner::SetHeader(outHeadersFiltered, L"Connection", L"Keep-Alive");
	}

	CFilterOwner::RemoveHeader(outHeadersFiltered, L"If-Modified-Since");
	CFilterOwner::RemoveHeader(outHeadersFiltered, L"If-None-Match");

	std::string sendOutBuf = "GET " + UTF8fromUTF16(filterOwner.url.getAfterHost()) + " HTTP/1.1" CRLF;
	for (auto& pair : outHeadersFiltered)
		sendOutBuf += UTF8fromUTF16(pair.first) + ": " + UTF8fromUTF16(pair.second) + CRLF;
	sendOutBuf += CRLF;

	// GET リクエストを送信
	if (sockWebsite->Write(sendOutBuf.c_str(), sendOutBuf.length()) == false)
		throw std::runtime_error("sockWebsite write error");

	// HTTP Header を受信する
	std::string buffer;
	GetHTTPHeader(filterOwner, sockWebsite.get(), buffer);

	if (filterOwner.responseLine.code != "200") {
		std::string sendInBuf = "HTTP/1.1 " + filterOwner.responseLine.code + " " + filterOwner.responseLine.msg + CRLF;
		for (auto& pair : filterOwner.inHeaders)
			sendInBuf += UTF8fromUTF16(pair.first) + ": " + UTF8fromUTF16(pair.second) + CRLF;
		sendInBuf += CRLF;

		// HTTP Header を送信
		WriteSocketBuffer(sockBrowser, sendInBuf.c_str(), sendInBuf.length());

		INFO_LOG << L"Send code[" << filterOwner.responseLine.code << "] : " << number;

	} else {
		std::string body = GetHTTPBody(filterOwner, sockWebsite.get(), buffer);
		funcSendThumb(body);

		if (bIncomplete == false) {
			CCritSecLock lock(g_csSaveThumb);

			std::wstring incompletePath = GetThumbCacheFolderPath() + number + L".jpg.incomplete";
			std::ofstream fscache(incompletePath, std::ios::out | std::ios::binary);
			fscache.write(body.c_str(), body.size());
			fscache.close();

			std::wstring completePath = GetThumbCacheFolderPath() + number + L".jpg";
			BOOL bRet = ::MoveFile(incompletePath.c_str(), completePath.c_str());
			if (bRet == 0) {
				ERROR_LOG << L"MoveFile failed : src : " << incompletePath << L" dest : " << completePath;
			}
		}
		INFO_LOG << L"Get and Send ThumbCache : " << number;
	}
}









