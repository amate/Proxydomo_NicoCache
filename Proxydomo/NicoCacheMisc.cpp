
#include "stdafx.h"
#include "NicoCacheMisc.h"
#include <regex>
#include <fstream>
#include <numeric>
#include "Misc.h"
#include "CodeConvert.h"
#include "NicoCacheManager.h"
#include "proximodo\util.h"
#include "WinHTTPWrapper.h"

using namespace CodeConvert;


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


/// ファイル作成時の無効な文字を置換する
void MtlValidateFileName(std::wstring& strName, LPCTSTR replaceChar /*= _T("-")*/)
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
		return *value;
	}

	return "";
}

std::string GetThumbURL(const std::string& smNumber)
{
	const std::string kGetThumbInfoURL = "http://ext.nicovideo.jp/api/getthumbinfo/";

	std::string getThumbURL = kGetThumbInfoURL + smNumber;
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























