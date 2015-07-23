/**
 *	@file	Misc.h
 *	@biref	���Ɣėp�I�ȎG���ȃ��[�`���Q
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
#ifndef MISC_H
#define MISC_H

#pragma once

#include <stdarg.h>
#include <io.h>
#include <vector>
#include <list>
#include <algorithm>

#include <atlstr.h>
#include <atlapp.h>
#define _WTL_NO_CSTRING
#include <atlmisc.h>

template <class _Function>
bool ForEachFile(const CString &strDirectoryPath, _Function __f)
{
	CString 		strPathFind = strDirectoryPath;

	::PathAddBackslash(strPathFind.GetBuffer(MAX_PATH));
	strPathFind.ReleaseBuffer();

	CString 		strPath = strPathFind;
	strPathFind += _T("*.*");

	WIN32_FIND_DATA wfd;
	HANDLE	h = ::FindFirstFile(strPathFind, &wfd);
	if (h == INVALID_HANDLE_VALUE)
		return false;

	// Now scan the directory
	do {
		// it is a file
		if ((wfd.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM)) == 0) {
			__f(strPath + wfd.cFileName);
		}
	} while (::FindNextFile(h, &wfd));

	::FindClose(h);

	return true;
}

template <class _Function>
bool ForEachFileWithAttirbutes(const CString &strDirectoryPath, _Function __f)
{
	CString 		strPathFind = strDirectoryPath;

	::PathAddBackslash(strPathFind.GetBuffer(MAX_PATH));
	strPathFind.ReleaseBuffer();

	CString 		strPath = strPathFind;
	strPathFind += _T("*.*");

	WIN32_FIND_DATA wfd;
	HANDLE	h = ::FindFirstFile(strPathFind, &wfd);
	if (h == INVALID_HANDLE_VALUE)
		return false;

	// Now scan the directory
	do {
		// it is a file
		if ((wfd.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM)) == 0) {
			__f(strPath + wfd.cFileName, wfd);
		}
	} while (::FindNextFile(h, &wfd));

	::FindClose(h);

	return true;
}

template <class _Function>
bool ForEachFileFolder(const CString &strDirectoryPath, _Function __f)
{
	CString 		strPathFind = strDirectoryPath;

	::PathAddBackslash(strPathFind.GetBuffer(MAX_PATH));
	strPathFind.ReleaseBuffer();

	CString 		strPath = strPathFind;
	strPathFind += _T("*.*");

	WIN32_FIND_DATA wfd;
	HANDLE	h = ::FindFirstFile(strPathFind, &wfd);
	if (h == INVALID_HANDLE_VALUE)
		return false;
	
	std::list<std::pair<std::wstring, bool>> vecFileFolder;
	// Now scan the directory
	do {
		if (::lstrcmp(wfd.cFileName, _T(".")) == 0 || ::lstrcmp(wfd.cFileName, _T("..")) == 0)
			continue;

		if ((wfd.dwFileAttributes & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM)) == 0) {
			vecFileFolder.emplace_back(wfd.cFileName, (wfd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0);
		}
	} while (::FindNextFile(h, &wfd));

	::FindClose(h);

	vecFileFolder.sort([](const std::pair<std::wstring, bool>& first, const std::pair<std::wstring, bool>& second) -> bool {
		return ::StrCmpLogicalW(first.first.c_str(), second.first.c_str()) < 0;
	});
	for (auto& pair : vecFileFolder)
		__f(strPath + pair.first.c_str(), pair.second);

	return true;
}


namespace Misc {

/// �N���b�v�{�[�h�ɂ���e�L�X�g���擾����
CString GetClipboardText(bool bUseOLE = false);
bool	SetClipboardText(const CString& str);

// ==========================================================================

//+++ �t�@�C���p�X�����A�t�@�C�������擾
const CString	GetFileBaseName(const CString& strFileName);

//+++ �t�@�C���p�X�����A�f�B���N�g�������擾. �Ō��'\\'�͊܂܂Ȃ�.
const CString	GetDirName(const CString& strFileName);

///+++ �t�@�C�����̊g���q�̎擾. �� ���ʂ̕�����ɂ�'.'�͊܂܂�Ȃ�.
const CString	GetFileExt(const CString& strFileName);

///+++ �t�H���_���g���q�����̃t�@�C�����̎擾. �� ���ʂ̕�����ɂ�'.'�͊܂܂�Ȃ�.
const CString	GetFileBaseNoExt(const CString& strFileName);

///+++ �g���q�����̃t�@�C�����̎擾. �� ���ʂ̕�����ɂ�'.'�͊܂܂�Ȃ�.
const CString	GetFileNameNoExt(const CString& strFileName);

//+++ ttp://��h�𑫂�����A���[�̋󔒂��폜�����肷��(SearchBar.h�̊֐����番��������������)
void	StrToNormalUrl(CString& strUrl);

/// ���Ȃ��t�@�C���p�X�ɂ��ĕԂ�
int	GetUniqueFilePath(CString& filepath, int nStart = 1);

//	strFile�� .bak �������t�@�C���ɃR�s�[. �Â� .bak ������΂���͍폜.
void	CopyToBackupFile(const CString& strFileName);


// ==========================================================================

///+++ undonut.exe�̃t���p�X����Ԃ�.  (MtlGetModuleFileName�ƈꏏ������...)
const CString 	GetExeFileName();

///+++ exe(dll)�̂���t�H���_��Ԃ�. �Ō��'\\'���t��
const CString 	GetExeDirectory();

///+++ exe(dll)�̂���t�H���_��Ԃ�. �Ō��'\\'���t���Ȃ�
const CString 	GetExeDirName();

///+++ �蔲���ȃt���p�X��. �f�B���N�g���̎w�肪�Ȃ���΁Aundonut�t�H���_���ƂȂ�.
const CString GetFullPath_ForExe(const CString& strFileName);

//------------------------------------------------------
CRect	GetMonitorWorkArea(HWND hWnd);


}	// namespace Misc


#endif
