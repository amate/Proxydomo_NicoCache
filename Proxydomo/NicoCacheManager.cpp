/**
*	@file	NicoCacheManager.cpp
*/

#include "stdafx.h"
#include "NicoCacheManager.h"
#include <regex>
#include <fstream>
#include <chrono>
#include <strstream>
#include <numeric>
#include <boost\format.hpp>
#include <Mmsystem.h>
#pragma comment(lib, "Winmm.lib")
#include <atlenc.h>
#include "proximodo\util.h"
#include "RequestManager.h"
#include "Logger.h"
#include "Misc.h"
#include "CodeConvert.h"
#include "MediaInfo.h"
#include "ptreeWrapper.h"
#include "timer.h"
#include "HttpOperate.h"
#include "NicoDatabase.h"
#include "WinHTTPWrapper.h"

using namespace CodeConvert;
using namespace std::chrono;

#define CR	'\r'
#define LF	'\n'
#define CRLF "\r\n"


namespace {

	CNicoDatabase	g_nicoDatabase;


	// 動画を保存するキャッシュフォルダのパスを返す(最後に'\'が付く)
	std::wstring GetCacheFolderPath()
	{
		return std::wstring(Misc::GetExeDirectory() + L"nico_cache\\");
	}

	// キャッシュフォルダからsmNumberの動画を探してパスを返す
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

	// サムネイルキャッシュフォルダのパスを返す(最後に'\'が付く)
	std::wstring GetThumbCacheFolderPath()
	{
		return std::wstring(Misc::GetExeDirectory() + L"nico_cache\\thumb_cache\\");
	}

	// サムネイルキャッシュフォルダからnumberのファイルを探してパスを返す
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

	// smNumberの保存先へのパスを返す(拡張子はつけない)
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
		strName = CUtil::replaceAll(strName, L"&quot;", L"'");
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

	CCriticalSection g_csReduceCache;

	// キャッシュした全動画のファイルサイズが一定以下になるように
	// 古い動画から削除していく
	void ReduceCache()
	{
		CCritSecLock lock(g_csReduceCache);

		std::list<std::pair<CString, WIN32_FIND_DATA>> cacheFileList;
		ForEachFileWithAttirbutes(GetCacheFolderPath().c_str(), [&cacheFileList](const CString& filePath, WIN32_FIND_DATA fd) {
			CString fileName = fd.cFileName;
			if (fileName.Left(1) == _T("#"))
				return;
			if (fileName.Left(2) != _T("sm"))
				return;

			cacheFileList.emplace_back(filePath, fd);
		});

		// ファイル作成日時の一番古いファイルを一番上にする
		cacheFileList.sort([](const std::pair<CString, WIN32_FIND_DATA>& first, const std::pair<CString, WIN32_FIND_DATA>& second) -> bool {
			auto funcFileTimeToUINT64 = [](FILETIME ft) {
				return static_cast<uint64_t>(ft.dwHighDateTime) << 32 | ft.dwLowDateTime;
			};
			uint64_t firstCreationTime = funcFileTimeToUINT64(first.second.ftCreationTime);
			uint64_t secondCreationTime = funcFileTimeToUINT64(second.second.ftCreationTime);
			return firstCreationTime < secondCreationTime;
		});

		auto funcFileSize = [](const WIN32_FIND_DATA& fd) {
			return static_cast<uint64_t>(fd.nFileSizeHigh) << 32 | fd.nFileSizeLow;
		};

		uint64_t totalCacheSize = std::accumulate(cacheFileList.begin(), cacheFileList.end(), static_cast<uint64_t>(0), 
			[funcFileSize](uint64_t sum, std::pair<CString, WIN32_FIND_DATA>& file) -> uint64_t {

				return sum + funcFileSize(file.second);
		});

		const uint64_t kMaxCacheSize = 30ui64 * 1024ui64 * 1024ui64 * 1024ui64;	// 30GB
		WCHAR strTotalCacheSize[64];
		::StrFormatByteSizeW(totalCacheSize, strTotalCacheSize, 64);
		WCHAR strMaxCacheSize[64];
		::StrFormatByteSizeW(kMaxCacheSize, strMaxCacheSize, 64);
		INFO_LOG << L"ReduceCache totalCacheSize : " << strTotalCacheSize << L" / " << strMaxCacheSize;

		if (kMaxCacheSize < totalCacheSize) {
			do {
				if (cacheFileList.empty())
					break;

				auto& cacheFile = cacheFileList.front();
				INFO_LOG << L"ReduceCache DeleteFile : " << (LPCTSTR)cacheFile.first;
				::DeleteFile(cacheFile.first);
				totalCacheSize -= funcFileSize(cacheFile.second);
				cacheFileList.erase(cacheFileList.begin());

			} while (kMaxCacheSize < totalCacheSize);
		}
		
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

}	// namespace


std::string	GetThumbData(const std::string& thumbURL)
{
	std::wstring url = UTF16fromUTF8(thumbURL);
	std::wregex rx(L"http://[^.]+\\.smilevideo\\.jp/smile\\?i=(.*)");
	std::wsmatch result;
	if (std::regex_match(url, result, rx) == false) {
		ATLASSERT(FALSE);
		return "";
	}

	std::wstring number = result.str(1);
	std::wstring thumbCachePath = GetThumbCachePath(number);

	if (thumbCachePath.length() > 0) {
		CString ext = Misc::GetFileExt(thumbCachePath.c_str());
		ext.MakeLower();
		if (ext != L"incomplete") {
			std::string thumbData = LoadFile(thumbCachePath);
			return thumbData;
		}
	}

	if (auto value = WinHTTPWrapper::HttpDownloadData(url.c_str())) {
		WinHTTPWrapper::TermWinHTTP();

		return *value;
	}

	return "";
}

std::string GetThumbURL(const std::string& smNumber)
{
	std::string getThumbURL = "http://ext.nicovideo.jp/api/getthumbinfo/" + smNumber;
	if (auto value = WinHTTPWrapper::HttpDownloadData(getThumbURL.c_str())) {
		std::string body = value.get();
		std::regex rx("<thumbnail_url>([^<]+)</thumbnail_url>");
		std::smatch result;
		if (std::regex_search(body, result, rx)) {
			std::string thumbURL = result[1].str();
			return thumbURL;
		}
	}
	ATLASSERT(FALSE);
	return "";
}

boost::optional<std::pair<int, int>>	GetDLCountAndClientDLCompleteCount(const std::string& smNumber)
{
	return g_nicoDatabase.GetDLCountAndClientDLCompleteCount(smNumber);
}

void	DownloadThumbDataWhereIsNULL()
{
	g_nicoDatabase.DownloadThumbDataWhereIsNULL();
}

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

	//CCritSecLock lock2(CNicoCacheManager::s_cssmNumberCacheManager);
	auto result = CNicoCacheManager::s_mapsmNumberCacheManager.emplace_front(
								std::make_pair(smNumber, std::unique_ptr<CNicoMovieCacheManager>(std::move(manager))));
	ATLASSERT(result.second);
	manager->m_thisThread = std::thread([manager]() {

		try {
			manager->Manage();
		}
		catch (std::exception& e) {
			ERROR_LOG << L"CNicoMovieCacheManager::StartThread : " << e.what();
			CNicoCacheManager::DestroyTransactionData(manager->m_transactionData);
		}

		manager->m_thisThread.detach();

		CCritSecLock lock(CNicoCacheManager::s_cssmNumberCacheManager);
		auto& mapList = CNicoCacheManager::s_mapsmNumberCacheManager.get<CNicoCacheManager::hash>();
		auto it = mapList.find(manager->m_smNumber);
		if (it == mapList.end()) {
			ATLASSERT(FALSE);
			return;
		}
		mapList.erase(it);
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

	//CCritSecLock lock2(CNicoCacheManager::s_cssmNumberCacheManager);
	auto result = CNicoCacheManager::s_mapsmNumberCacheManager.emplace_front(
		std::make_pair(smNumber, std::unique_ptr<CNicoMovieCacheManager>(std::move(manager))));
	ATLASSERT(result.second);
	manager->m_thisThread = std::thread([manager]() {

		try {
			manager->Manage();
		}
		catch (std::exception& e) {
			ERROR_LOG << L"CNicoMovieCacheManager::StartThread : " << e.what();
			CNicoCacheManager::DestroyTransactionData(manager->m_transactionData);
		}

		manager->m_thisThread.detach();

		CCritSecLock lock(CNicoCacheManager::s_cssmNumberCacheManager);
		auto& mapList = CNicoCacheManager::s_mapsmNumberCacheManager.get<CNicoCacheManager::hash>();
		auto it = mapList.find(manager->m_smNumber);
		if (it == mapList.end()) {
			ATLASSERT(FALSE);
			return;
		}
		mapList.erase(it);
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
	WriteSocketBuffer(browserRangeRequest.sockBrowser.get(), sendInBuf.c_str(), sendInBuf.length());
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

	std::wstring title = CNicoCacheManager::Get_smNumberTitle(m_smNumber);
	if (g_nicoDatabase.AddNicoHistory(m_smNumber, UTF8fromUTF16(title))) {
		std::string thumbURL = CNicoCacheManager::Get_smNumberThumbURL(m_smNumber);
		if (thumbURL.empty()) {
			thumbURL = GetThumbURL(m_smNumber);
		}
		ATLASSERT(thumbURL.length() > 0);
		std::string thumbData = GetThumbData(thumbURL + ".M");
		if (thumbData.empty()) {
			thumbData = GetThumbData(thumbURL);
		}
		if (thumbData.length() > 0) {
			g_nicoDatabase.SetThumbData(m_smNumber, thumbData.data(), static_cast<int>(thumbData.size()));
		} else {
			ATLASSERT(FALSE);
		}
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
		HeadPairList outHeaders;
		if (m_optNicoRequestData) {	// DLのみ
			url = m_optNicoRequestData->url;
			outHeaders = m_optNicoRequestData->outHeaders;
		} else {
			url = m_browserRangeRequestList.front().filterOwner.url;
			outHeaders = m_browserRangeRequestList.front().filterOwner.outHeaders;
		}

		// 送信ヘッダを編集
		auto outHeadersFiltered = outHeaders;
		if (bIncomplete && m_movieCacheBuffer.size() > 0) {
			// Range ヘッダを変更して途中からデータを取得する
			std::wstring range = (boost::wformat(L"bytes=%1%-") % m_movieCacheBuffer.size()).str();
			CFilterOwner::SetHeader(outHeadersFiltered, L"Range", range);
		} else {
			// Range ヘッダを削ってファイル全体を受信する設定にする
			CFilterOwner::RemoveHeader(outHeadersFiltered, L"Range");
		}

		CFilterOwner::RemoveHeader(outHeadersFiltered, L"If-Modified-Since");
		CFilterOwner::RemoveHeader(outHeadersFiltered, L"If-None-Match");

		// リクエストを送信
		m_sockWebsite = SendRequest(url, HttpVerb::kHttpGet, outHeadersFiltered);

		// レスポンスヘッダ を受信する
		std::string buffer;
		CFilterOwner filterOwner;
		GetResponseHeader(filterOwner, m_sockWebsite.get(), buffer);
		if (filterOwner.responseLine.code != "200" && filterOwner.responseLine.code != "206") {
			// ブラウザへ接続エラーを通知する
			std::string sendInBuf = "HTTP/1.1 " + filterOwner.responseLine.code + " " + filterOwner.responseLine.msg + CRLF;
			std::string name;
			for (auto& pair : filterOwner.inHeaders)
				sendInBuf += UTF8fromUTF16(pair.first) + ": " + UTF8fromUTF16(pair.second) + CRLF;

			sendInBuf += CRLF;
			sendInBuf += buffer;
			if (m_browserRangeRequestList.size() > 0) {
				WriteSocketBuffer(m_browserRangeRequestList.front().sockBrowser.get(), sendInBuf.c_str(), sendInBuf.length());
				m_browserRangeRequestList.front().sockBrowser->Close();
				m_browserRangeRequestList.erase(m_browserRangeRequestList.begin());
			}

			m_sockWebsite->Close();
			m_sockWebsite.reset();

			// 履歴に書き込み
			CCritSecLock lock(m_transactionData->csData);
			m_transactionData->detailText.Format(L"接続エラー code : %d", std::stoi(filterOwner.responseLine.code));

			CNicoCacheManager::AddDLedNicoCacheManager(m_smNumber, m_transactionData);
			CNicoCacheManager::DestroyTransactionData(m_transactionData);

			ERROR_LOG << m_smNumber << L" 接続エラー code : " << filterOwner.responseLine.code;
			return;
		}

		// ファイルサイズ取得
		if (bIncomplete && m_movieCacheBuffer.size() > 0) {
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
		m_transactionData->oldDLPos = m_lastMovieSize;

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
	bool isAddDLedList = false;
	bool clientDLComplete = false;
	steady_clock::time_point lastTime;
	boost::optional<steady_clock::time_point> optboostTimeCountStart = steady_clock::now();
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

						if (bLowRequest == false || m_bReserveVideoConvert) {
							if (CNicoCacheManager::VideoConveter(m_smNumber, cacheFileManager.CompleteCachePath(), m_bReserveVideoConvert)) {
								CCritSecLock lock(m_transactionData->csData);
								m_transactionData->detailText = _T("エンコードを開始します。");
								++m_transactionData->lastDLPos;
							}
						}
					}
					{
						CCritSecLock lock(m_transactionData->csData);
						m_transactionData->detailText = L"ダウンロードを完了しました！";
					}
					CNicoCacheManager::AddDLedNicoCacheManager(m_smNumber, m_transactionData);
					isAddDLedList = true;
					CNicoCacheManager::ConsumeDLQue();
				}
			}
		} else {
			if (debugSizeCheck == false) {
				if (m_movieCacheBuffer.size() != m_movieSize) {
					ERROR_LOG << L"サイトとの接続が切れたが、ファイルをすべてDLできませんでした...";

					if (_RetryDownload(nicoRequestData, fs)) {
						continue;
					}
				}
				if (isAddDLedList == false) {
					{
						CCritSecLock lock(m_transactionData->csData);
						if (m_movieCacheBuffer.size() != m_movieSize) {
							m_transactionData->detailText = L"サイトとの接続が切れたが、ファイルをすべてDLできませんでした...";
						} else {
							m_transactionData->detailText = L"ダウンロードを完了しました！";
						}
					}
					CNicoCacheManager::AddDLedNicoCacheManager(m_smNumber, m_transactionData);
					isAddDLedList = true;
				}
				if (m_sockWebsite) {
					m_sockWebsite->Close();
					m_sockWebsite.reset();
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

						WriteSocketBuffer(browserRangeRequest.sockBrowser.get(), 
											m_movieCacheBuffer.c_str() + browserRangeRequest.rangeBufferPos, sendSize);
						browserRangeRequest.rangeBufferPos += sendSize;

						auto browserData = m_transactionData->GetBrowserTransactionData(static_cast<void*>(&browserRangeRequest));
						browserData->rangeBufferPos = browserRangeRequest.rangeBufferPos;

						if (browserRangeRequest.rangeBufferPos > browserRangeRequest.browserRangeEnd) {	// 終わり
							INFO_LOG << browserRangeRequest.GetID() << L" browser close";
							browserRangeRequest.sockBrowser->Close();
							suicideList.emplace_back(browserRangeRequest.itThis);

							if (browserRangeRequest.rangeBufferPos == m_movieSize && clientDLComplete == false) {
								g_nicoDatabase.ClientDLComplete(m_smNumber);
								clientDLComplete = true;
							}
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

		// 10秒経ったら一度切って再接続する
		if (optboostTimeCountStart && m_sockWebsite && m_sockWebsite->IsConnected() && fs &&
			((steady_clock::now() - *optboostTimeCountStart) > seconds(10))) 
		{
			INFO_LOG << m_smNumber << L" boost retry download";
			_RetryDownload(nicoRequestData, fs);
			optboostTimeCountStart.reset();
		}
	}	// for(;;)

	CNicoCacheManager::DestroyTransactionData(m_transactionData);

	ReduceCache();

	INFO_LOG << m_smNumber << L" Manage finish";
}

bool	CNicoMovieCacheManager::_RetryDownload(const NicoRequestData& nicoRequestData, std::ofstream& fs)
{
	++m_retryCount;
	INFO_LOG << L"_RetryDownload : retryCount : " << m_retryCount;

	if (kMaxRetryCount < m_retryCount) {
		ERROR_LOG << L"Reached maxRetryCount";
		return false;
	}

	m_sockWebsite->Close();
	m_sockWebsite.reset();

	CUrl url = nicoRequestData.url;
	HeadPairList outHeaders = nicoRequestData.outHeaders;

	// 送信ヘッダを編集
	auto outHeadersFiltered = outHeaders;

	// Range ヘッダを変更して途中からデータを取得する
	std::wstring range = (boost::wformat(L"bytes=%1%-") % m_movieCacheBuffer.size()).str();
	CFilterOwner::SetHeader(outHeadersFiltered, L"Range", range);

	CFilterOwner::RemoveHeader(outHeadersFiltered, L"If-Modified-Since");
	CFilterOwner::RemoveHeader(outHeadersFiltered, L"If-None-Match");

	// リクエストを送信
	m_sockWebsite = SendRequest(url, HttpVerb::kHttpGet, outHeadersFiltered);

	// レスポンスヘッダ を受信する
	std::string buffer;
	CFilterOwner filterOwner;
	GetResponseHeader(filterOwner, m_sockWebsite.get(), buffer);

	// ファイルサイズ取得
	std::wstring contentRange = filterOwner.GetInHeader(L"Content-Range");
	std::wregex rx(L"bytes \\d+-\\d+/(\\d+)");
	std::wsmatch result;
	if (std::regex_search(contentRange, result, rx) == false)
		throw std::runtime_error("contentRange not found");

	int64_t movieSize = boost::lexical_cast<int64_t>(result.str(1));
	ATLASSERT(movieSize == m_movieSize);

	INFO_LOG << m_smNumber << L" ファイルサイズ : " << m_movieSize;

	fs.write(buffer.c_str(), buffer.size());
	m_movieCacheBuffer += buffer;
	m_lastMovieSize = m_movieCacheBuffer.size();
	m_transactionData->lastDLPos = m_lastMovieSize;

	return true;
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

CCriticalSection CNicoCacheManager::s_csvecDLedNicoCacheManager;
std::list<DLedNicoCache> CNicoCacheManager::s_vecDLedNicoCacheManager;

CCriticalSection CNicoCacheManager::s_csvideoConvertList;
std::list<VideoConvertItem> CNicoCacheManager::s_videoConvertList;

CCriticalSection CNicoCacheManager::s_cs_smNumberThumbURL;
std::unordered_map<std::string, std::string>	CNicoCacheManager::s_map_smNumberThumbURL;


CCriticalSection CNicoCacheManager::s_csCacheGetThumbInfo;
std::pair<std::string, std::string> CNicoCacheManager::s_cacheGetThumbInfo;

CCriticalSection CNicoCacheManager::s_csCacheWatchPage;
std::pair<std::string, std::string> CNicoCacheManager::s_cacheWatchPage;

CCriticalSection CNicoCacheManager::s_csCacheGetflv;
std::pair<std::string, std::string> CNicoCacheManager::s_cacheGetflv;

CCriticalSection CNicoCacheManager::s_csCacheCommentList;
std::pair<std::string, NicoCommentList> CNicoCacheManager::s_cacheCommentList;

CCriticalSection CNicoCacheManager::s_csLastOutHeaders;
HeadPairList CNicoCacheManager::s_lastOutHeaders;


void CNicoCacheManager::CreateNicoConnectionFrame()
{
	s_nicoConnectionFrame.reset(new CNicoConnectionFrame);
	s_nicoConnectionFrame->Create(NULL);

	ReduceCache();
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
		std::string smNumber;
		if (query.length() > 0) {
			smNumber = query.substr(3);	// ?v=smXXX
		} else {
			std::string url = UTF8fromUTF16(filterOwner.url.getUrl());
			auto slashPos = url.rfind('/');
			ATLASSERT(slashPos != std::string::npos);
			smNumber = url.substr(slashPos + 1);
		}

		CCritSecLock lock(s_csCacheGetflv);
		if (s_cacheGetflv.first != smNumber) {

			std::string body = GetHTTPPage(filterOwner);
			if (filterOwner.responseLine.code != "200")
				throw std::runtime_error("responseCode not 200 : " + filterOwner.responseCode);

			std::string sendInBuf = SendResponse(filterOwner, sockBrowser, body);

			// movieURL を取得
			std::string escBody = CUtil::UESC(body);
			std::regex rx("url=([^&]+)");
			std::smatch result;
			if (std::regex_search(escBody, result, rx) == false)
				throw std::runtime_error("url not found");

			std::string movieURL = result.str(1);
			INFO_LOG << L"smNumber : " << smNumber << L" movieURL : " << movieURL;

			Associate_movieURL_smNumber(movieURL, smNumber);

			s_cacheGetflv.first = smNumber;
			s_cacheGetflv.second = sendInBuf;

		} else {
			// キャッシュを送信
			WriteSocketBuffer(sockBrowser, s_cacheGetflv.second.c_str(), s_cacheGetflv.second.length());
			INFO_LOG << smNumber << L" Send CacheGetflv";
		}
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
		//ATLASSERT(url.getUrl().find(L"nicovideo.jp/smile") == std::wstring::npos);
		if (url.getUrl().find(L"nicovideo.jp/smile") != std::wstring::npos) {
			ERROR_LOG << L"movieURL trap failed : " << url.getUrl();
		}
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
		// キャッシュが存在していれば同時DLを止めない
		CCacheFileManager cacheManager(smNumber, false);
		if (cacheManager.SearchCache() == false) {

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

			{	// キューに入っていれば削除する
				CCritSecLock lock3(s_cssmNumberDLQue);
				auto& hashList = s_smNumberDLQue.get<hash>();
				auto it = hashList.find(smNumber);
				if (it != hashList.end()) {
					hashList.erase(it);
				}
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
	//ATLASSERT(it != hashList.end());
	if (it != hashList.end()) {
		std::string movieURL = it->movieURL;
		return movieURL;
	} else {
		return "";
	}

}

void CNicoCacheManager::Associate_smNumberThumbURL(const std::string& smNumber, const std::string& thumbURL)
{
	ATLASSERT(thumbURL.size() > 0);
	CCritSecLock lock(s_cs_smNumberThumbURL);
	s_map_smNumberThumbURL.insert(std::make_pair(smNumber, thumbURL));
}

std::string CNicoCacheManager::Get_smNumberThumbURL(const std::string& smNumber)
{
	auto it = s_map_smNumberThumbURL.find(smNumber);
	if (it != s_map_smNumberThumbURL.end()) {
		return it->second;
	}
	ATLASSERT(FALSE);
	return "";
}

extern const std::wstring kGetThumbInfoURL;

std::wstring CNicoCacheManager::FailFound(const std::string& smNumber)
{
	std::wstring requestThumbInfoURL = kGetThumbInfoURL + UTF16fromUTF8(smNumber);

	CFilterOwner filterOwner;
	filterOwner.url.parseUrl(requestThumbInfoURL);
	filterOwner.SetOutHeader(L"Host", filterOwner.url.getHost());

	std::string thumbInfoBody = GetHTTPPage(filterOwner);
	if (thumbInfoBody.empty()) {
		ERROR_LOG << L"FailFound fail : thumbInfoBody empty";
		return L"";
	}

	auto thumbInfoTree = ptreeWrapper::BuildPtreeFromText(UTF16fromUTF8(thumbInfoBody));
	auto& thumbTree = thumbInfoTree.get_child(L"nicovideo_thumb_response.thumb");

	using boost::property_tree::wptree;
	wptree videoInfoTree;

	wptree videoTree;
	videoTree.add(L"id", thumbTree.get<std::wstring>(L"video_id"));
	videoTree.add(L"user_id", thumbTree.get<std::wstring>(L"user_id"));
	videoTree.add(L"deleted", L"0");
	videoTree.add(L"description", thumbTree.get<std::wstring>(L"description"));
	std::wstring length = thumbTree.get<std::wstring>(L"length");
	std::wregex rx(L"(\\d+):(\\d+)");
	std::wsmatch result;
	if (std::regex_match(length, result, rx) == false) {
		ERROR_LOG << L"FailFound fail : length no rx match";
		return L"";
	}
	int min = std::stoi(result.str(1));
	int sec = std::stoi(result.str(2));
	int length_in_seconds = min * 60 + sec;
	videoTree.add(L"length_in_seconds", std::to_wstring(length_in_seconds));
	videoTree.add(L"thumbnail_url", thumbTree.get<std::wstring>(L"thumbnail_url"));
	videoTree.add(L"upload_time", thumbTree.get<std::wstring>(L"first_retrieve"));	// none
	videoTree.add(L"first_retrieve", thumbTree.get<std::wstring>(L"first_retrieve"));
	videoTree.add(L"view_counter", thumbTree.get<std::wstring>(L"view_counter"));
	videoTree.add(L"mylist_counter", thumbTree.get<std::wstring>(L"mylist_counter"));
	videoTree.add(L"option_flag_community", L"0");	// none
	videoTree.add(L"option_flag_nicowari", L"0");	// none
	videoTree.add(L"option_flag_middle_thumbnail", L"1");	// none

	videoTree.add(L"options.<xmlattr>.adult", L"0");	// none
	videoTree.add(L"options.<xmlattr>.large_thumbnail", L"1");	// none
	videoTree.add(L"options.<xmlattr>.sun", L"0");	// none
	videoTree.add(L"options.<xmlattr>.mobile", L"0");	// none

	videoInfoTree.add_child(L"video", videoTree);

	auto& thumbTagsTree = thumbTree.get_child(L"tags");
	std::wstring domain = thumbTree.get<std::wstring>(L"tags.<xmlattr>.domain");
	wptree tagsTree;
	for (auto& tag : thumbTagsTree) {
		if (tag.first == L"tag") {
			wptree tag_info;
			tag_info.add(L"tag", tag.second.get_value<std::wstring>());
			tag_info.add(L"area", domain);
			tagsTree.add_child(L"tag_info", tag_info);
		}
	}
	videoInfoTree.add_child(L"tags", tagsTree);

	std::wstringstream ss;
	boost::property_tree::write_xml(ss, videoInfoTree);
	std::wstring videoInfoContent = ss.str();
	std::wregex rx2(L"<\\?xml[^>]+>\n");
	videoInfoContent = std::regex_replace(videoInfoContent, rx2, L"", std::regex_constants::format_first_only);
	return videoInfoContent;
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
	CCritSecLock lock2(CNicoCacheManager::s_cssmNumberCacheManager);
	CNicoMovieCacheManager::StartThread(requestData.first, requestData.second);
}


//////////////////////////////////////////////////
/// クッキー管理クラス

class CCookieManager
{
public:

	void	SetCookie(const std::wstring& data)
	{
		std::wregex rx(LR"((\w+)=([^;]+)(?: |;)?)");
		std::wsmatch result;
		auto itbegin = data.cbegin();
		auto itend = data.cend();
		while (std::regex_search(itbegin, itend, result, rx)) {
			std::wstring name = result[1].str();
			if (name != _T("expires") && name != _T("path") && name != _T("domain")) {
				std::wstring value = result[2].str();
				m_mapCookie[name] = value;
			}
			itbegin = result[0].second;
		}
	}

	CString	GetCookie() const
	{
		CString cookie = _T("Cookie: ");
		for (auto it = m_mapCookie.cbegin(); it != m_mapCookie.cend(); ++it) {
			cookie.AppendFormat(_T("%s=%s;"), it->first.c_str(), it->second.c_str());
		}
		return cookie;
	}

	CString GetRawData() const
	{
		return GetCookie().Mid(8);
	}

private:
	std::map<std::wstring, std::wstring>	m_mapCookie;
};


void CNicoCacheManager::QueDownloadVideo(const std::wstring& watchPageURL)
{
	std::wregex rx(LR"(^http://www.nicovideo.jp/watch/(\w+\d+))");
	std::wsmatch result;
	if (std::regex_search(watchPageURL, result, rx) == false)
		return;

	std::string smNumber = UTF8fromUTF16(result.str(1));
	std::wstring cachePath = Get_smNumberFilePath(smNumber + "_");
	if (cachePath.empty())
		cachePath = Get_smNumberFilePath("#" + smNumber + "_");

	if (cachePath.length() > 0) {
		CString ext = Misc::GetFileExt(cachePath.c_str());
		ext.MakeLower();
		if (ext != _T("incomplete"))
			return;	// 不完全ファイル以外のキャッシュがあればダウンロードしない
	}
	std::thread([smNumber]() {
		{
			CCritSecLock lock(s_cssmNumberDLQue);
			auto& hashList = s_smNumberDLQue.get<hash>();
			auto it = hashList.find(smNumber);
			if (it != hashList.end())
				return;	// 既にDLキューに入ってる
		}
		{
			CCritSecLock lock(s_cssmNumberCacheManager);
			auto& hashList = s_mapsmNumberCacheManager.get<hash>();
			auto it = hashList.find(smNumber);
			if (it != hashList.end())
				return;	// 既にダウンロード中である
		}

		HeadPairList outHeaders;
		{
			CCritSecLock lock(s_csLastOutHeaders);
			if (s_lastOutHeaders.empty()) {
				ERROR_LOG << L"no lastOutHeaders";
				return;
			}
			outHeaders = s_lastOutHeaders;
		}
		CFilterOwner::RemoveHeader(outHeaders, L"Referer");

		{
			CFilterOwner filterOwner;
			std::wstring watchPageURL = L"http://www.nicovideo.jp/watch/" + UTF16fromUTF8(smNumber);
			filterOwner.url.parseUrl(watchPageURL);
			filterOwner.outHeaders = outHeaders;
			filterOwner.SetOutHeader(L"Host", filterOwner.url.getHost());
			filterOwner.SetOutHeader(L"User-Agent", L"Mozilla/5.0 (Windows NT 6.1; Win64; x64; Trident/7.0; rv:11.0) like Gecko");
			std::string body = GetHTTPPage(filterOwner);

			std::wstring utf16body = UTF16fromUTF8(body);
			std::wregex rx(LR"(<span class="originalVideoTitle">([^<]+)</span>)");
			std::wsmatch result;
			if (std::regex_search(utf16body, result, rx) == false) {
				ERROR_LOG << L"no title found";
				return;
			}

			std::wstring title = result.str(1);
			Associate_smNumberTitle(smNumber, title);

			// Cookie更新
			CCookieManager cookieManager;
			cookieManager.SetCookie(filterOwner.GetOutHeader(L"Cookie"));
			for (auto& pair : filterOwner.inHeaders) {
				if (CUtil::noCaseEqual(pair.first, L"Set-Cookie")) {
					cookieManager.SetCookie(pair.second);
				}
			}
			CFilterOwner::SetHeader(outHeaders, L"Cookie", (LPCWSTR)cookieManager.GetRawData());
		}

		std::wstring movieURL;
		////////////////////////////////////////////////
		{
			CFilterOwner filterOwner;
			std::wstring getflvURL = L"http://flapi.nicovideo.jp/api/getflv/" + UTF16fromUTF8(smNumber);
			filterOwner.url.parseUrl(getflvURL);
			filterOwner.outHeaders = outHeaders;
			filterOwner.SetOutHeader(L"Host", filterOwner.url.getHost());
			std::string body = GetHTTPPage(filterOwner);
			if (body.empty()) {
				ERROR_LOG << L"getflv no body";
				return;
			}

			// movieURL を取得
			std::string escBody = CUtil::UESC(body);
			std::regex rx("url=([^&]+)");
			std::smatch result;
			if (std::regex_search(escBody, result, rx) == false) {
				ERROR_LOG << L"url not found";
				return;
			}

			std::string movieURLtie = result.str(1);
			INFO_LOG << L"smNumber : " << smNumber << L" movieURL : " << movieURLtie;

			Associate_movieURL_smNumber(movieURLtie, smNumber);

			movieURL = UTF16fromUTF8(movieURLtie);
		}
#if 0
		/////////////////////////////////////////////////
		if (Get_smNumberTitle(smNumber).empty()) {
			CFilterOwner filterOwner;
			std::wstring getthumbinfoURL = L"http://ext.nicovideo.jp/api/getthumbinfo/" + UTF16fromUTF8(smNumber);
			filterOwner.url.parseUrl(getthumbinfoURL);
			filterOwner.outHeaders = outHeaders;
			filterOwner.SetOutHeader(L"Host", filterOwner.url.getHost());
			std::string body = GetHTTPPage(filterOwner);

			std::wstring utf16body = UTF16fromUTF8(body);
			std::wregex rx(L"<title>([^<]+)</title>");
			std::wsmatch result;
			if (std::regex_search(utf16body, result, rx) == false) {
				ERROR_LOG << L"no title found";
				return;
			}

			std::wstring title = result.str(1);
			Associate_smNumberTitle(smNumber, title);
		}
#endif
		{
			CCritSecLock lock(s_cssmNumberCacheManager);
			auto& hashList = s_mapsmNumberCacheManager.get<hash>();
			auto it = hashList.find(smNumber);
			if (it != hashList.end())
				return;	// 既にダウンロード中である

			NicoRequestData nicoRequestData;
			nicoRequestData.url.parseUrl(movieURL);
			nicoRequestData.outHeaders = outHeaders;
			CFilterOwner::SetHeader(nicoRequestData.outHeaders, L"Host", nicoRequestData.url.getHost());

			if (s_mapsmNumberCacheManager.size() < kMaxParallelDLCount) {
				CNicoMovieCacheManager::StartThread(smNumber, nicoRequestData);
			} else {
				AddDLQue(smNumber, nicoRequestData);
			}
		}
	}).detach();
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

		HeadPairList inHeaders;
		CFilterOwner::SetHeader(inHeaders, L"Content-Type", L"image/jpeg");
		CFilterOwner::SetHeader(inHeaders, L"Content-Length", std::to_wstring(fileContent.size()));
		CFilterOwner::SetHeader(inHeaders, L"Connection", L"keep-alive");

		SendHTTPOK_Response(sockBrowser.get(), inHeaders, fileContent);
	};

	bool bIncomplete = false;
	if (thumbCachePath.length() > 0) {
		CString ext = Misc::GetFileExt(thumbCachePath.c_str());
		ext.MakeLower();
		if (ext != L"incomplete") {
			std::string fileContent = LoadFile(thumbCachePath);
			funcSendThumb(fileContent);

			//INFO_LOG << L"Send ThumbCache : " << number;
			return;

		} else {
			INFO_LOG << L"Send ThumbCache incomplete found : " << number;
			bIncomplete = true;
		}
	}

	// 送信ヘッダを編集
	auto outHeadersFiltered = filterOwner.outHeaders;
	CFilterOwner::RemoveHeader(outHeadersFiltered, L"If-Modified-Since");
	CFilterOwner::RemoveHeader(outHeadersFiltered, L"If-None-Match");

	// リクエストを送信
	auto sockWebsite = SendRequest(filterOwner.url, HttpVerb::kHttpGet, outHeadersFiltered);

	// レスポンスヘッダを受信する
	std::string buffer;
	GetResponseHeader(filterOwner, sockWebsite.get(), buffer);
	std::string body = GetResponseBody(filterOwner, sockWebsite.get(), buffer);
	sockWebsite->Close();

	if (filterOwner.responseLine.code != "200") {
		SendResponse(filterOwner, sockBrowser.get(), body);

		//INFO_LOG << L"Send code[" << filterOwner.responseLine.code << "] : " << number;

	} else {
		if (body.empty()) {
			filterOwner.responseLine.code = "404";
			filterOwner.responseLine.msg = "Not Found";

			SendResponse(filterOwner, sockBrowser.get(), "");

			//INFO_LOG << L"Send body empty" << number;

		} else {
			funcSendThumb(body);

			if (bIncomplete == false) {
				CCritSecLock lock(g_csSaveThumb);

				std::wstring incompletePath = GetThumbCacheFolderPath() + number + L".jpg.incomplete";
				std::ofstream fscache(incompletePath, std::ios::out | std::ios::binary);
				fscache.write(body.c_str(), body.size());
				fscache.close();

				std::wstring completePath = GetThumbCacheFolderPath() + number + L".jpg";
				BOOL bRet = ::MoveFileEx(incompletePath.c_str(), completePath.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
				if (bRet == 0) {
					ERROR_LOG << L"MoveFileEx failed : src : " << incompletePath << L" dest : " << completePath;
				}
			}
			//INFO_LOG << L"Get and Send ThumbCache : " << number;
		}
	}
}


void CNicoCacheManager::AddDLedNicoCacheManager(const std::string& smNumber, std::shared_ptr<TransactionData> transData)
{	
	CCritSecLock lock2(s_csvecDLedNicoCacheManager);
	CCritSecLock lock3(transData->csData);
	if (s_vecDLedNicoCacheManager.size() > 0) {
		auto& front = s_vecDLedNicoCacheManager.front();
		if (front.smNumber == smNumber && front.name == transData->name && front.description == (LPCWSTR)transData->detailText)
			return;	// 重複したら追加しない
	}
	s_vecDLedNicoCacheManager.emplace_front(transData->name, smNumber, (LPCWSTR)transData->detailText);
}


const std::wstring kNicoCacheServerURL = L"http://local.ptron/nicocache/nicocache_api";

bool CNicoCacheManager::IsNicoCacheServerRequest(const CUrl& url)
{
	if (url.getUrl().compare(0, kNicoCacheServerURL.length(), kNicoCacheServerURL) != 0)
		return false;

	return true;
}

void CNicoCacheManager::ManageNicoCacheServer(CFilterOwner& filterOwner, std::unique_ptr<CSocket>& sockBrowser)
{
	
	std::wstring query = filterOwner.url.getQuery();
	if (query.find(L"nicoHistory") != std::wstring::npos) {
		auto nicoHistoryList = g_nicoDatabase.QueryNicoHistoryList();

		timer processTimer;
		std::string json;
		json += R"({"NicoHistory":[)";
		for (auto it = nicoHistoryList.cbegin(); it != nicoHistoryList.cend(); ++it) {
			auto& nicoHistory = *it;
			json += "{";
			json += R"("smNumber":")" + nicoHistory.smNumber + R"(",)";
			json += R"("title":")" + nicoHistory.title + R"(",)";
			json += R"("DLCount":")" + std::to_string(nicoHistory.DLCount) + R"(",)";
			json += R"("ClientDLCompleteCount":")" + std::to_string(nicoHistory.ClientDLCompleteCount) + R"(",)";
			json += R"("lastAccessTime":")" + nicoHistory.lastAccessTime + R"(",)";

			if (nicoHistory.thumbData.size() > 0) {
				int base64length = Base64EncodeGetRequiredLength(static_cast<int>(nicoHistory.thumbData.size()), ATL_BASE64_FLAG_NOCRLF);
				std::vector<char> base64image(base64length);
				BOOL bRet = Base64Encode((const BYTE*)nicoHistory.thumbData.data(), static_cast<int>(nicoHistory.thumbData.size()), base64image.data(), &base64length, ATL_BASE64_FLAG_NOCRLF);
				ATLASSERT(bRet);
				ATLASSERT(base64length == base64image.size());

				json += R"("thumbData":"data:image/jpeg;base64,)";
				json.append(base64image.data(), base64image.size());
				json += R"(")";
			} else {
				json += R"("thumbData":"")";
			}

			json += "}";
			if (std::next(it) != nicoHistoryList.cend()) {
				json += ",";
			}
		}
		json += R"(]})";

		INFO_LOG << L"nicocache_api?nicoHistory " << processTimer.format();

		HeadPairList inHeaders;
		CFilterOwner::SetHeader(inHeaders, L"Content-Type", L"application/json; charset=utf-8");
		CFilterOwner::SetHeader(inHeaders, L"Content-Length", std::to_wstring(json.size()));
		CFilterOwner::SetHeader(inHeaders, L"Connection", L"keep-alive");

		SendHTTPOK_Response(sockBrowser.get(), inHeaders, json);
		return;
	}
	auto pos = query.find(L"videoConvert_smNumber=");
	if (pos != std::wstring::npos) {
		std::string convert_smNumber = UTF8fromUTF16(query.substr(pos + wcslen(L"videoConvert_smNumber=")));
		ATLASSERT(convert_smNumber.length());

		std::wstring filePath = Get_smNumberFilePath(convert_smNumber + "_");
		CString ext = Misc::GetFileExt(filePath.c_str());
		ext.MakeLower();
		if (filePath.length() > 0 && ext != L"incomplete") {
			VideoConveter(convert_smNumber, filePath, true);

		} else {
			CCritSecLock lock(s_cssmNumberCacheManager);
			auto& hashList = s_mapsmNumberCacheManager.get<hash>();
			auto it = hashList.find(convert_smNumber);
			ATLASSERT(it != hashList.end());
			if (it == hashList.end()) {
				ERROR_LOG << L"manager not found : " << convert_smNumber;
			} else {
				it->second->ReserveVideoConvert();
			}
		}

		std::string sendBody = "VideoConvert request complete!";

		HeadPairList inHeaders;
		CFilterOwner::SetHeader(inHeaders, L"Content-Type", L"text/html");
		CFilterOwner::SetHeader(inHeaders, L"Content-Length", std::to_wstring(sendBody.size()));
		CFilterOwner::SetHeader(inHeaders, L"Connection", L"keep-alive");

		SendHTTPOK_Response(sockBrowser.get(), inHeaders, sendBody);

	} else {
		std::wstring json;
		json = L"{";
		{
			json += L"\"DLingItems\": [";
			CCritSecLock lock(s_cssmNumberCacheManager);
			auto& list = s_mapsmNumberCacheManager.get<seq>();
			for (auto it = s_mapsmNumberCacheManager.begin(); it != s_mapsmNumberCacheManager.end(); ++it) {
				auto& pair = *it;
				auto transData = pair.second->m_transactionData;
				CCritSecLock lock2(transData->csData);
				json += L"{";
				json += L"\"name\": \"" + transData->name + L"\",";
				json += L"\"smNumber\": \"" + UTF16fromUTF8(pair.first) + L"\",";
				int progress = static_cast<int>((static_cast<double>(transData->lastDLPos) / static_cast<double>(transData->fileSize)) * 100.0);
				json += L"\"progress\": " + std::to_wstring(progress) + L",";
				json += L"\"description\": \"" + transData->detailText + L"\"";
				json += L"}";
				if (std::next(it) != s_mapsmNumberCacheManager.end()) {
					json += L",";
				}
			}
			json += L"], ";
		}
		{
			json += L"\"VideoConvertItems\": [";
			CCritSecLock lock(s_csvideoConvertList);
			for (auto it = s_videoConvertList.begin(); it != s_videoConvertList.end(); ++it) {
				auto& convertItem = *it;
				json += L"{";
				json += L"\"name\": \"" + convertItem.name + L"\",";
				json += L"\"progress\": \"" + convertItem.progress + L"\"";
				json += L"}";
				if (std::next(it) != s_videoConvertList.end()) {
					json += L",";
				}
			}
			json += L"], ";
		}
		{
			json += L"\"DLedItems\": [";
			CCritSecLock lock(s_csvecDLedNicoCacheManager);
			for (auto it = s_vecDLedNicoCacheManager.begin(); it != s_vecDLedNicoCacheManager.end(); ++it) {
				auto& DLedData = *it;

				json += L"{";
				json += L"\"name\": \"" + DLedData.name + L"\",";
				json += L"\"smNumber\": \"" + UTF16fromUTF8(DLedData.smNumber) + L"\",";
				json += L"\"description\": \"" + DLedData.description + L"\"";
				json += L"}";
				if (std::next(it) != s_vecDLedNicoCacheManager.end()) {
					json += L",";
				}
			}
			json += L"]";
		}
		json += L"}";

		std::string sendBody = UTF8fromUTF16(json);

		HeadPairList inHeaders;
		CFilterOwner::SetHeader(inHeaders, L"Content-Type", L"application/json; charset=utf-8");
		CFilterOwner::SetHeader(inHeaders, L"Content-Length", std::to_wstring(sendBody.size()));
		CFilterOwner::SetHeader(inHeaders, L"Connection", L"keep-alive");

		SendHTTPOK_Response(sockBrowser.get(), inHeaders, sendBody);
	}
}


bool	CNicoCacheManager::VideoConveter(const std::string& smNumber, const std::wstring& filePath, bool bForceConvert /*= false*/)
{
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

	auto funcIsNeedConvert = [&]() -> bool {
#if 0
		// 参照フレームが6以上 かつ 解像度が 1280x720以上 なら必ずコンバートする
		// sm26706342_【Minecraft】 こつこつクラフト+ G Part1 【弦巻マキ実況】
		if (6 < videoInfo->ref_frames && (1280 <= videoInfo->width && 720 <= videoInfo->height))
			return true;
#endif

		// level 5 かつ fpsが 30 以上なら
		if (level[0] == L'5' && 30.0 < videoInfo->fps) {
			return true;
		}

		// 1280x720 以下で参照フレームが9以下なら大丈夫？
		if ((videoInfo->width <= 1280 && videoInfo->height <= 720) && videoInfo->ref_frames <= 9) {
			return false;
		}
#if 0
		// sm26702656_【7 Days to Die】やることは後から考えよう！Part2 の設定？ 最初のあたりで止まってしまう
		enum { kMaxReFrames = 5, kBadweightb = 2 };
		if (kMaxReFrames <= videoInfo->ref_frames && videoInfo->weightb == kBadweightb) {
			return true;
		}
#endif		
		return false;
	};

	if (funcIsNeedConvert() || bForceConvert) {

		CCritSecLock lock(s_csvideoConvertList);
		std::wstring name = (LPCWSTR)Misc::GetFileBaseNoExt(filePath.c_str());
		s_videoConvertList.emplace_front(name);
		auto itThis = s_videoConvertList.begin();
		lock.Unlock();

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

		std::thread([smNumber, filePath, destResolution, name, itThis]() {
			try {
				CHandle hReadPipe;
				CHandle hWritePipe;
				if (!::CreatePipe(&hReadPipe.m_h, &hWritePipe.m_h, nullptr, 0))
					throw std::runtime_error("CreatePipe failed");

				CHandle hStdOutput;
				if (!::DuplicateHandle(GetCurrentProcess(), hWritePipe, GetCurrentProcess(), &hStdOutput.m_h, 0, TRUE, DUPLICATE_SAME_ACCESS))
					throw std::runtime_error("DuplicateHandle failed");

				hWritePipe.Close();

				CString qsvEncPath = Misc::GetExeDirectory() + _T("QSVEncC\\x64\\QSVEncC64.exe");
				std::wstring title = CNicoCacheManager::Get_smNumberTitle(smNumber);
				std::wstring outPath = GetCacheFolderPath() + UTF16fromUTF8(smNumber) + L"enc_" + title + L".mp4";
				CString commandLine =
					(boost::wformat(L"--avqsv --copy-audio --cqp 30 --quality 1 --i-adapt --b-adapt --output-res %1%x%2% -i \"%3%\" -o \"%4%\"")
					% destResolution.cx % destResolution.cy % filePath % outPath).str().c_str();
				STARTUPINFO startUpInfo = { sizeof(STARTUPINFO) };
				startUpInfo.dwFlags = STARTF_USESTDHANDLES;
				//startUpInfo.hStdOutput = hStdOutput;
				startUpInfo.hStdError = hStdOutput;
				PROCESS_INFORMATION processInfo = {};

				INFO_LOG << L"VideoConveter EncStart : " << smNumber << L" commandLine : " << (LPCWSTR)commandLine;

				BOOL bRet = ::CreateProcess(qsvEncPath, (LPWSTR)(LPCWSTR)commandLine,
					nullptr, nullptr, TRUE, 0, nullptr, nullptr, &startUpInfo, &processInfo);
				//BOOL bRet = ::CreateProcess(_T("C:\\Windows\\System32\\ipconfig.exe"), nullptr,
				//	nullptr, nullptr, TRUE, CREATE_NEW_CONSOLE, nullptr, nullptr, &startUpInfo, &processInfo);
				ATLASSERT(bRet);

				hStdOutput.Close();

				std::string outresult;

				for (;;) {
					enum { kBufferSize = 512 };
					char buffer[kBufferSize + 1] = "";
					DWORD readSize = 0;
					bRet = ::ReadFile(hReadPipe, (LPVOID)buffer, kBufferSize, &readSize, nullptr);
					if (bRet && readSize == 0)  { // EOF
						break;
					}

					if (bRet == 0) {
						DWORD err = ::GetLastError();
						if (err == ERROR_BROKEN_PIPE) {
							break;
						}
					}
					outresult.append(buffer, readSize);

					for (;;) {
						auto nPos = outresult.find_first_of("\r\n");
						if (nPos != std::string::npos) {
							std::string line = outresult.substr(0, nPos);
							outresult.erase(0, nPos + 1);

							if (line.length() > 0 && line[0] == '[') {
								auto closePos = line.find(']');
								if (closePos != std::string::npos) {
									std::string progress = line.substr(1, closePos - 1);
									//INFO_LOG << L"VideoConvert progress : " << progress;
									CCritSecLock lock(itThis->csData);
									itThis->progress = UTF16fromUTF8(progress);
								}
							}
						} else {
							break;
						}
					}
				}

				hReadPipe.Close();

				::WaitForSingleObject(processInfo.hProcess, INFINITE);
				::CloseHandle(processInfo.hThread);
				::CloseHandle(processInfo.hProcess);

				std::ifstream fs(outPath, std::ios::in | std::ios::binary);
				if (!fs)
					throw std::runtime_error("outPath open failed");

				fs.seekg(0, std::ios::end);
				streamoff fileSize = fs.tellg();
				fs.close();

				bool bSuccess = false;
				if (fileSize == 0) {
					::DeleteFile(outPath.c_str());
					ERROR_LOG << L"VideoConveter Encode failed";

				} else {
					std::wstring backupPath = GetCacheFolderPath() + UTF16fromUTF8(smNumber) + L"org_" + title + L".mp4";
					bRet = ::MoveFileEx(filePath.c_str(), backupPath.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
					if (bRet) {
						bRet = ::MoveFileEx(outPath.c_str(), filePath.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
						if (bRet) {
							// 完了！
							INFO_LOG << L"VideoConveter EncFinish! : " << smNumber;
							bSuccess = true;

							{
								CCritSecLock lock2(s_cssmNumberCacheManager);
								auto& list = s_mapsmNumberCacheManager.get<hash>();
								auto it = list.find(smNumber);
								if (it != list.end()) {
									it->second->SwitchToInvalid();
								}
							}
							CString SEpath = Misc::GetExeDirectory() + _T("宝箱出現.wav");
							ATLVERIFY(::PlaySound(SEpath, NULL, SND_FILENAME | SND_ASYNC));

						} else {
							ERROR_LOG << L"MoveFileEx failed : src : " << outPath << L" dest : " << filePath;
						}
					} else {
						ERROR_LOG << L"MoveFileEx failed : src : " << filePath << L" dest : " << backupPath;
					}
				}
				{
					CCritSecLock lock2(s_csvecDLedNicoCacheManager);
					s_vecDLedNicoCacheManager.emplace_front(name, smNumber,
						bSuccess ? L"動画の変換を完了しました。！" : L"動画の変換に失敗しました...");
				}
				{
					CCritSecLock lock(s_csvideoConvertList);
					s_videoConvertList.erase(itThis);
				}
			}
			catch (std::exception& e) {
				ERROR_LOG << L"VideoConvertThread exception : " << e.what();
			}
		}).detach();

		return true;
	} else {
		return false;
	}
}



const std::wstring kGetThumbInfoURL = L"http://ext.nicovideo.jp/api/getthumbinfo/";
const std::wstring kWatchPageURL = L"http://www.nicovideo.jp/watch/";
const std::wstring ksoWatchPageURL = L"http://www.nicovideo.jp/watch/so";
const std::wstring kmsgServerURL = L"http://msg.nicovideo.jp/";
const std::wstring kiNicovideoURL = L"http://i.nicovideo.jp/v3/video.array";

bool CNicoCacheManager::ManagePostCache(CFilterOwner& filterOwner, std::unique_ptr<CSocket>& sockBrowser, std::string& recvOutBuf)
{
	if (filterOwner.url.getUrl().compare(0, kGetThumbInfoURL.length(), kGetThumbInfoURL) == 0) {
		// http://ext.nicovideo.jp/api/getthumbinfo/ をハンドルする
		std::string smNumber = UTF8fromUTF16(filterOwner.url.getUrl().substr(kGetThumbInfoURL.length()));

		CCritSecLock lock(s_csCacheGetThumbInfo);
		if (s_cacheGetThumbInfo.first != smNumber) {

			std::string body = GetHTTPPage(filterOwner);
			std::regex rx("<thumbnail_url>([^<]+)</thumbnail_url>");
			std::smatch result;
			if (std::regex_search(body, result, rx)) {
				std::string thumbURL = result[1].str();
				Associate_smNumberThumbURL(smNumber, thumbURL);
			} else {
				ATLASSERT(FALSE);
			}

			std::string sendInBuf = SendResponse(filterOwner, sockBrowser.get(), body);

			s_cacheGetThumbInfo.first = smNumber;
			s_cacheGetThumbInfo.second = sendInBuf;

		} else {
			// キャッシュを送信
			WriteSocketBuffer(sockBrowser.get(), s_cacheGetThumbInfo.second.c_str(), s_cacheGetThumbInfo.second.length());
			INFO_LOG << smNumber << L" Send CacheThumbInfo";
		}
		return true;
	} else if (filterOwner.url.getUrl().compare(0, kWatchPageURL.length(), kWatchPageURL) == 0) {
		{
			CCritSecLock lock(s_csLastOutHeaders);
			s_lastOutHeaders = filterOwner.outHeaders;
		}

		// http://www.nicovideo.jp/watch/xxx をハンドルする
		if (filterOwner.url.getQuery() != L"?watch_harmful=1") {
			return false;	// iNicoからではない
		}
		if (filterOwner.url.getUrl().compare(0, ksoWatchPageURL.length(), ksoWatchPageURL) == 0) {
			return false;	// watch/soXXX
		}

		auto quesPos = filterOwner.url.getUrl().find(L"?watch_harmful=1");
		std::string smNumber = UTF8fromUTF16(filterOwner.url.getUrl().substr(kWatchPageURL.length(), quesPos - kWatchPageURL.length()));

		CCritSecLock lock(s_csCacheWatchPage);
		if (s_cacheWatchPage.first != smNumber) {
			std::string body = GetHTTPPage(filterOwner);

			std::wstring utf16body = UTF16fromUTF8(body);
			std::wregex rx(L"<p id=\"video_title\"><!-- google_ad_section_start -->([^<]+)<!-- google_ad_section_end -->");
			std::wsmatch result;
			if (std::regex_search(utf16body, result, rx)) {
				std::wstring title = result.str(1);
				Associate_smNumberTitle(smNumber, title);
			} else {
				ATLASSERT(FALSE);
			}
			std::string sendInBuf = SendResponse(filterOwner, sockBrowser.get(), body);

			s_cacheWatchPage.first = smNumber;
			s_cacheWatchPage.second = sendInBuf;

		} else {
			// キャッシュを送信
			WriteSocketBuffer(sockBrowser.get(), s_cacheWatchPage.second.c_str(), s_cacheWatchPage.second.length());
			INFO_LOG << smNumber << L" Send CacheWatchPage";
		}
		return true;

	} else if (filterOwner.url.getUrl().compare(0, kiNicovideoURL.length(), kiNicovideoURL) == 0) {
		// 動画読み込み時にコメントキャッシュをクリアしておく
		CCritSecLock lock(s_csCacheCommentList);
		s_cacheCommentList.first.clear();
		s_cacheCommentList.second.inHeaders.clear();
		s_cacheCommentList.second.commentListBody.clear();
		s_cacheCommentList.second.inOwnerHeaders.clear();
		s_cacheCommentList.second.commentListOwnerBody.clear();
		return false;

	} else if (filterOwner.url.getUrl().compare(0, kmsgServerURL.length(), kmsgServerURL) == 0) {
		// メッセージサーバーとの通信をハンドルする
		std::wstring contentLength = filterOwner.GetOutHeader(L"Content-Length");
		if (contentLength.empty()) {
			return false;
		}

		timer timer;

		// POSTデータの中身を読みだす
		int64_t postSize = boost::lexical_cast<int64_t>(contentLength);
		if (recvOutBuf.size() != postSize) {
			for (;;) {
				bool bRead = ReadSocketBuffer(sockBrowser.get(), recvOutBuf);
				if (bRead == false) {
					::Sleep(50);
				}
				if (recvOutBuf.size() == postSize)
					break;

				if (sockBrowser->IsConnected() == false)
					throw std::runtime_error("sockBrowser connection close");
			}
		}
		std::wstring postBody = UTF16fromUTF8(recvOutBuf);
		INFO_LOG << L"#" << filterOwner.requestNumber << L" Post Body : " << postBody;

		std::regex rx("thread=\"(\\d+)\"");
		std::smatch result;
		if (std::regex_search(recvOutBuf, result, rx) == false) {
			ERROR_LOG << L"no thread number";
			throw std::runtime_error("no thread number");
		}

		// コメント投稿の場合
		std::wregex rx2(L"<chat [^>]+vpos=\"(\\d+)\"[^>]+>([^<]+)</chat>");
		std::wsmatch result2;
		bool bPostComment = false;
		std::string vpos;
		std::string comment;
		if (std::regex_search(postBody, result2, rx2)) {
			bPostComment = true;
			vpos = UTF8fromUTF16(result2.str(1));
			comment = UTF8fromUTF16(result2.str(2));
		}

		std::string threadNumber = result.str(1);
		bool bOwnerComment = recvOutBuf.find("fork=\"1\"") != std::string::npos;
		CCritSecLock lock(s_csCacheCommentList);
		if (s_cacheCommentList.first != threadNumber) {
			s_cacheCommentList.second.inHeaders.clear();
			s_cacheCommentList.second.commentListBody.clear();
			s_cacheCommentList.second.inOwnerHeaders.clear();
			s_cacheCommentList.second.commentListOwnerBody.clear();
			s_cacheCommentList.first = threadNumber;

		} else if (bPostComment == false) {
			// キャッシュを利用する
			std::string& body = bOwnerComment ? s_cacheCommentList.second.commentListOwnerBody : s_cacheCommentList.second.commentListBody;
			if (body.length() > 0) {
				HeadPairList& inHeaders = bOwnerComment ? s_cacheCommentList.second.inOwnerHeaders : s_cacheCommentList.second.inHeaders;
				// レスポンスを送信
				SendHTTPOK_Response(sockBrowser.get(), inHeaders, body);

				INFO_LOG << threadNumber << L" Send CacheComment : ownerComment : " << bOwnerComment;
				return true;
			}
		}

		auto funcPostAndGet = [&](const std::string& postData) -> std::string 
		{
			// 送信ヘッダを編集
			auto outHeadersFiltered = filterOwner.outHeaders;
			CFilterOwner::SetHeader(outHeadersFiltered, L"Content-Length", std::to_wstring(postData.length()));

			// POSTリクエストを送信
			auto sockWebsite = SendRequest(filterOwner.url, HttpVerb::kHttpPost, outHeadersFiltered);

			// Post内容を送信
			WriteSocketBuffer(sockWebsite.get(), postData.c_str(), postData.length());

			// HTTP Header を受信する
			std::string buffer;
			filterOwner.inHeaders.clear();
			GetResponseHeader(filterOwner, sockWebsite.get(), buffer);
			// Body を受信する
			std::string body = GetResponseBody(filterOwner, sockWebsite.get(), buffer);
			sockWebsite->Close();
			return body;
		};

		std::string body = funcPostAndGet(recvOutBuf);

		//std::wstring utf16body = UTF16fromUTF8(body);
		//INFO_LOG << L"#" << filterOwner.requestNumber << L" Post Response : " << utf16body;

		// キャッシュを更新する
		if (bPostComment == false) {
			std::string& cacheBody = bOwnerComment ? s_cacheCommentList.second.commentListOwnerBody : s_cacheCommentList.second.commentListBody;
			cacheBody = body;
			HeadPairList& inHeaders = bOwnerComment ? s_cacheCommentList.second.inOwnerHeaders : s_cacheCommentList.second.inHeaders;
			inHeaders = filterOwner.inHeaders;
		}

		// 投稿コメントをコメントキャッシュに反映する
		if (bPostComment) {
			/*
			niconicoのメッセージ(コメント)サーバーのタグや送り方の説明 - 神の味噌汁青海
			http://blog.goo.ne.jp/hocomodashi/e/3ef374ad09e79ed5c50f3584b3712d61

			status: 投稿ステータス

			0 = SUCCESS(投稿完了)
			1 = FAILURE(投稿拒否)
			2 = INVALID_THREAD(スレッドIDがおかしい)
			3 = INVALID_TICKET(投稿チケットが違う)
			4 = INVALID_POSTKEY(ポストキーがおかしい or ユーザーIDがおかしい)
			5 = LOCKED(コメントはブロックされている)
			6 = READONLY(コメントは書き込めない)
			8 = TOO_LONG(コメント内容が長すぎる)
			*/
			auto funcSuccessThenAppendComment = [&](const boost::property_tree::wptree& resultTree) {
				std::string& commentList = s_cacheCommentList.second.commentListBody;
				if (commentList.empty()) {
					ERROR_LOG << L"no cache comment";

				} else {
					// コメントリストを更新
					int no = resultTree.get<int>(L"packet.chat_result.<xmlattr>.no");
					std::string last_res = "last_res=\"" + std::to_string(no) + "\"";
					std::regex rx4("last_res=\"\\d+\"");
					commentList = std::regex_replace(commentList, rx4, last_res, std::regex_constants::format_first_only);

					std::string lastComment = (boost::format("<chat thread=\"%1%\" no=\"%2%\" vpos=\"%3%\" date=\"%4%\" mail=\"184\" user_id=\"test\" anonymity=\"1\">%5%</chat></packet>") % threadNumber % no % vpos % time(nullptr) % comment).str();
					boost::replace_last(commentList, "</packet>", lastComment);

					CFilterOwner::SetHeader(s_cacheCommentList.second.inHeaders, L"Content-Length", std::to_wstring(commentList.size()));
				}
			};

			std::wstring utf16body = UTF16fromUTF8(body);
			auto resultTree = ptreeWrapper::BuildPtreeFromText(utf16body);
			int status = resultTree.get<int>(L"packet.chat_result.<xmlattr>.status");
			if (status != 0) {
				ERROR_LOG << L"Post Comment failed : " << body;
				if (status == 3) {
					INFO_LOG << L"status == 3 do retry";
					std::string retryThread = UTF8fromUTF16(resultTree.get<std::wstring>(L"packet.chat_result.<xmlattr>.thread"));
					std::string retryPostBody = "<thread res_from=\"-1\" version=\"20061206\" scores=\"1\" thread=\"" + retryThread + "\" />";
					std::string retryResult = funcPostAndGet(retryPostBody);
					if (retryResult.empty()) {
						ERROR_LOG << L"retry PostAndGet failed";

					} else {
						std::wstring utf16RetryResult = UTF16fromUTF8(retryResult);
						auto retryResultTree = ptreeWrapper::BuildPtreeFromText(utf16RetryResult);
						std::wstring ticket = retryResultTree.get<std::wstring>(L"packet.thread.<xmlattr>.ticket");

						std::string& commentList = s_cacheCommentList.second.commentListBody;
						std::string replaceTicket = "ticket=\"" + UTF8fromUTF16(ticket) + "\"";
						std::regex rx5("ticket=\"[^\"]+\"");
						commentList = std::regex_replace(commentList, rx5, replaceTicket, std::regex_constants::format_first_only);

						CFilterOwner::SetHeader(s_cacheCommentList.second.inHeaders, L"Content-Length", std::to_wstring(commentList.size()));

						INFO_LOG << L"ticket replaced : " << ticket;

						// コメント書き込みのリトライ
						recvOutBuf = std::regex_replace(recvOutBuf, rx5, replaceTicket, std::regex_constants::format_first_only);
						INFO_LOG << L"retry postComment : " << UTF16fromUTF8(recvOutBuf);
						body = funcPostAndGet(recvOutBuf);
						if (body.empty()) {
							ERROR_LOG << L"retry response body empty";

						} else {
							INFO_LOG << L"retry response body : " << body;

							utf16body = UTF16fromUTF8(body);
							resultTree = ptreeWrapper::BuildPtreeFromText(utf16body);
							int retrystatus = resultTree.get<int>(L"packet.chat_result.<xmlattr>.status");
							if (retrystatus != 0) {
								ERROR_LOG << L"retry failed status : " << status;

							} else {
								funcSuccessThenAppendComment(resultTree);
							}
						}
					}
				}
			} else {
				funcSuccessThenAppendComment(resultTree);
			}
		}

		// レスポンスを送信
		SendResponse(filterOwner, sockBrowser.get(), body);

		INFO_LOG << L"thread : " << threadNumber << L" postComment : " << bPostComment << L" " << timer.format();
		return true;
	}

	return false;
}






