#pragma once

#include <string>
#include "Misc.h"
#include "Logger.h"

// 動画を保存するキャッシュフォルダのパスを返す(最後に'\'が付く)
std::wstring GetCacheFolderPath();

// キャッシュフォルダからsmNumberの動画を探してパスを返す
std::wstring Get_smNumberFilePath(const std::string& smNumber);

// smNumberの保存先へのパスを返す(拡張子はつけない)
std::wstring CreateCacheFilePath(const std::string& smNumber, bool bLowReqeust);


// サムネイルキャッシュフォルダのパスを返す(最後に'\'が付く)
std::wstring GetThumbCacheFolderPath();

// サムネイルキャッシュフォルダからnumberのファイルを探してパスを返す
std::wstring GetThumbCachePath(const std::wstring& number);


/// ファイル作成時の無効な文字を置換する
void MtlValidateFileName(std::wstring& strName, LPCTSTR replaceChar = _T("-"));


std::string	GetThumbData(const std::string& thumbURL);
std::string GetThumbURL(const std::string& smNumber);


// ファイル内容を読み込み
std::string LoadFile(const std::wstring& filePath);

// キャッシュした全動画のファイルサイズが一定以下になるように
// 古い動画から削除していく
void ReduceCache();

/////////////////////////////////////////////////////////////////////
// CCacheFileManager

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
