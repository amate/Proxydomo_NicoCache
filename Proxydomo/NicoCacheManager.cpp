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
#include "NicoCacheMisc.h"

using namespace CodeConvert;
using namespace std::chrono;

#define CR	'\r'
#define LF	'\n'
#define CRLF "\r\n"


namespace {


}	// namespace


boost::optional<std::pair<int, int>>	GetDLCountAndClientDLCompleteCount(const std::string& smNumber)
{
	return CNicoDatabase::GetInstance().GetDLCountAndClientDLCompleteCount(smNumber);
}

void	DownloadThumbDataWhereIsNULL()
{
	CNicoDatabase::GetInstance().DownloadThumbDataWhereIsNULL();
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
			WriteSocketBuffer(sockBrowser, s_cacheGetflv.second.c_str(), static_cast<int>(s_cacheGetflv.second.length()));
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
						INFO_LOG << it->second->smNumber() << L" をリストから取り除きました";
						it->second->ForceDatach();
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
		NicoMoviewCacheStartData startData(smNumber , filterOwner, std::move(sockBrowser));
		CNicoMovieCacheManager::StartThread(startData);
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
	CCritSecLock lock(s_cs_smNumberThumbURL);
	auto it = s_map_smNumberThumbURL.find(smNumber);
	if (it != s_map_smNumberThumbURL.end()) {
		return it->second;
	}
	std::string thumbURL = GetThumbURL(smNumber);
	if (thumbURL.length() > 0) {
		s_map_smNumberThumbURL.insert(std::make_pair(smNumber, thumbURL));
		return thumbURL;
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
	NicoMoviewCacheStartData startData(requestData.first, requestData.second);
	CNicoMovieCacheManager::StartThread(startData);
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
				NicoMoviewCacheStartData startData(smNumber, nicoRequestData);
				CNicoMovieCacheManager::StartThread(startData);
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
	auto nicoHistoryPos = query.find(L"nicoHistory");
	if (nicoHistoryPos != std::wstring::npos) {
		NicoListQuery queryOrder = NicoListQuery::kDownloadOrderDesc;
		auto orderPos = query.find(L"&order=");
		if (orderPos != std::wstring::npos) {
			std::wstring strOrder = query.substr(orderPos + wcslen(L"&order="));
			if (strOrder == L"DownloadOrderDesc") {
				queryOrder = NicoListQuery::kDownloadOrderDesc;
			} else if (strOrder == L"ClientDLIncompleteOrderDesc") {
				queryOrder = NicoListQuery::kClientDLIncompleteOrderDesc;
			} else if (strOrder == L"LastAccessTimerOrderDesc") {
				queryOrder = NicoListQuery::kLastAccessTimerOrderDesc;
			} else {
				ATLASSERT(FALSE);
			}
		}
		auto nicoHistoryList = CNicoDatabase::GetInstance().QueryNicoHistoryList(queryOrder);

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

		//INFO_LOG << L"nicocache_api?nicoHistory " << processTimer.format();

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
									INFO_LOG << L"VideoConvert success and Do ForceDatatch " << smNumber;
									it->second->ForceDatach();
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
			WriteSocketBuffer(sockBrowser.get(), s_cacheGetThumbInfo.second.c_str(), static_cast<int>(s_cacheGetThumbInfo.second.length()));
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
			WriteSocketBuffer(sockBrowser.get(), s_cacheWatchPage.second.c_str(), static_cast<int>(s_cacheWatchPage.second.length()));
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
			WriteSocketBuffer(sockWebsite.get(), postData.c_str(), static_cast<int>(postData.length()));

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






