// Proxydomo.cpp : main source file for Proxydomo.exe
//
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
#include <locale.h>
#include <atlframe.h>
#include <atlctrls.h>
#include <atldlgs.h>

#include "resource.h"

#include "MainDlg.h"
#include "Socket.h"
#include "Proxy.h"
#include "Settings.h"
#include "VersionControl.h"
#include "Logger.h"
#include "ssl.h"
#include "UITranslator.h"


// グローバル変数
CAppModule _Module;


int Run(LPTSTR /*lpstrCmdLine*/ = NULL, int nCmdShow = SW_SHOWDEFAULT)
{
	CMessageLoop theLoop;
	_Module.AddMessageLoop(&theLoop);

	CMainDlg dlgMain;

	if(dlgMain.Create(NULL) == NULL)
	{
		ATLTRACE(_T("Main dialog creation failed!\n"));
		return 0;
	}

	//dlgMain.ShowWindow(nCmdShow);

	int nRet = theLoop.Run();

	_Module.RemoveMessageLoop();
	return nRet;
}

int WINAPI _tWinMain(HINSTANCE hInstance, HINSTANCE /*hPrevInstance*/, LPTSTR lpstrCmdLine, int nCmdShow)
{
	HRESULT hRes = ::CoInitialize(NULL);
// If you are running on NT 4.0 or higher you can use the following call instead to 
// make the EXE free threaded. This means that calls come in on a random RPC thread.
//	HRESULT hRes = ::CoInitializeEx(NULL, COINIT_MULTITHREADED);
	ATLASSERT(SUCCEEDED(hRes));

#ifdef _DEBUG
	// ATLTRACEで日本語を使うために必要
	_tsetlocale( LC_ALL, _T("japanese") );
#endif

	// this resolves ATL window thunking problem when Microsoft Layer for Unicode (MSLU) is used
	::DefWindowProc(NULL, 0, 0, 0L);

	AtlInitCommonControls(ICC_BAR_CLASSES);	// add flags to support other controls

    // リッチエディットコントロール初期化
    HINSTANCE hRich = LoadLibrary(CRichEditCtrl::GetLibraryName());
    if(hRich == NULL){
        AtlMessageBox(NULL, _T("リッチエディットコントロール初期化失敗"),
            _T("エラー"), MB_OK | MB_ICONERROR);
        return 0;
    }

	hRes = _Module.Init(NULL, hInstance);
	ATLASSERT(SUCCEEDED(hRes));

	CVersionControl::Run();

	CSettings::LoadSettings();
	UITranslator::LoadUILanguage();

	CSocket::Init();
	
	CSettings::s_SSLFilter = InitSSL();

	CProxy	proxy;
	proxy.OpenProxyPort(CSettings::s_proxyPort);

	int nRet = Run(lpstrCmdLine, nCmdShow);

	proxy.CloseProxyPort();

	if (CSettings::s_SSLFilter)
		TermSSL();

	CSocket::Term();

	CSettings::SaveSettings();

	FreeLibrary(hRich);

	_Module.Term();
	::CoUninitialize();

	return nRet;
}
