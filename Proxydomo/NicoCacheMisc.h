#pragma once

#include <string>
#include "Misc.h"
#include "Logger.h"

// �����ۑ�����L���b�V���t�H���_�̃p�X��Ԃ�(�Ō��'\'���t��)
std::wstring GetCacheFolderPath();

// �L���b�V���t�H���_����smNumber�̓����T���ăp�X��Ԃ�
std::wstring Get_smNumberFilePath(const std::string& smNumber);

// smNumber�̕ۑ���ւ̃p�X��Ԃ�(�g���q�͂��Ȃ�)
std::wstring CreateCacheFilePath(const std::string& smNumber, bool bLowReqeust);


// �T���l�C���L���b�V���t�H���_�̃p�X��Ԃ�(�Ō��'\'���t��)
std::wstring GetThumbCacheFolderPath();

// �T���l�C���L���b�V���t�H���_����number�̃t�@�C����T���ăp�X��Ԃ�
std::wstring GetThumbCachePath(const std::wstring& number);


/// �t�@�C���쐬���̖����ȕ�����u������
void MtlValidateFileName(std::wstring& strName, LPCTSTR replaceChar = _T("-"));


std::string	GetThumbData(const std::string& thumbURL);
std::string GetThumbURL(const std::string& smNumber);


// �t�@�C�����e��ǂݍ���
std::string LoadFile(const std::wstring& filePath);

// �L���b�V�������S����̃t�@�C���T�C�Y�����ȉ��ɂȂ�悤��
// �Â����悩��폜���Ă���
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
			INFO_LOG << L"�Â��L���b�V�����폜���܂����B : " << m_lowCachePath;
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
