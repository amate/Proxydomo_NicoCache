/**
*	@file	TransactionView.cpp
*/

#include "stdafx.h"
#include "TransactionView.h"
#include <boost\format.hpp>
#include <boost\property_tree\ptree.hpp>
#include <boost\property_tree\ini_parser.hpp>
#include <VersionHelpers.h>
#include <vsstyle.h>
#include "Misc.h"
#include "Logger.h"
#include "UITranslator.h"
using namespace UITranslator;

using namespace boost::property_tree;


////////////////////////////////////////////////////////////////////////
// CTransactionView

CTransactionView::CTransactionView()
{
}

CTransactionView::~CTransactionView()
{
}


std::shared_ptr<TransactionData>	CTransactionView::CreateTransactionData(const std::wstring& name)
{
	bool timerStart = false;
	auto transData = std::make_shared<TransactionData>(name);
	{
		CCritSecLock lock(m_csTransDataList);
		transData->funcRefreshList = [this]() {
			_RefreshList();
		};
		timerStart = m_transDataList.empty();
		m_transDataList.emplace_front(transData);
		transData->itThis = m_transDataList.begin();
	}
	_RefreshList();
	if (timerStart) {
		SetTimer(kUpdateListTimerId, kUpdateListTimerInterval);
		m_dwLastTime = ::timeGetTime();
	}
	return transData;
}

void	CTransactionView::DestroyTransactionData(std::shared_ptr<TransactionData> transData)
{
	{
		CCritSecLock lock(m_csTransDataList);
		m_transDataList.erase(transData->itThis);

		if (m_transDataList.empty()) {
			if (IsWindow()) {
				KillTimer(kUpdateListTimerId);
			}
		}
	}
	if (IsWindow()) {
		_RefreshList();
	}
}

void	CTransactionView::_RefreshList()
{
	CRect rcClient;
	GetClientRect(rcClient);

	int cySize = 0;
	CCritSecLock lock(m_csTransDataList);
	for (auto& data : m_transDataList) {
		CCritSecLock lock2(data->csData);
		// アイテムの位置を設定
		data->rcItem.top = cySize;
		cySize += ItemHeight;
		data->rcItem.bottom = cySize;
		cySize += UnderLineHeight;

		data->rcItem.right = rcClient.right;

		for (auto& browserTransactionData : data->browserTransactionList) {
			CRect& rcItem = browserTransactionData.rcItem;
			rcItem.top = cySize;
			cySize += cyBrowserHeight;
			rcItem.bottom = cySize;

			cySize += UnderLineHeight;

			rcItem.left = cxBrowserLeftMargin;
			rcItem.right = rcClient.right;
		}
	}

	CSize	size;
	size.cx = rcClient.right;
	size.cy = m_transDataList.size() ? cySize : 1;
	lock.Unlock();

	SetScrollSize(size, TRUE, false);
	SetScrollLine(10, 10);

	Invalidate(FALSE);
}

void	CTransactionView::_UpdateListInfo()
{
	// 更新
	DWORD dwNowTime = ::timeGetTime();
	DWORD dwTimeMargin = dwNowTime - m_dwLastTime;
	if (dwTimeMargin <= 0) {
		return;
	} else {
		m_dwLastTime = dwNowTime;
	}

	int	i = 0;
	CCritSecLock lock(m_csTransDataList);
	for (auto it = m_transDataList.begin(); it != m_transDataList.end(); ++it) {
		auto& data = *it->get();

		CCritSecLock lock2(data.csData);

		if (data.fileSize != 0 && data.lastDLPos == data.fileSize) {
			data.detailText = _T("ダウンロードを完了しました！");

		} else if (data.fileSize < data.lastDLPos) {
			// Encodeing...

		} else {
			// (1.2 MB/sec)
			CString strTransferRate;
			int64_t nProgressMargin = data.lastDLPos - data.oldDLPos;
			data.oldDLPos = data.lastDLPos;

			if (data.deqProgressAndTime.size() >= 10)
				data.deqProgressAndTime.pop_front();
			data.deqProgressAndTime.push_back(std::make_pair(nProgressMargin, dwTimeMargin));
			nProgressMargin = 0;
			DWORD nTotalTime = 0;
			for (auto itq = data.deqProgressAndTime.cbegin(); itq != data.deqProgressAndTime.cend(); ++itq) {
				nProgressMargin += itq->first;
				nTotalTime += itq->second;
			}

			ATLASSERT(nTotalTime > 0);
			// kbyte / second
			double dKbTransferRate = (double)nProgressMargin / (double)nTotalTime;	// kbyte / second
			double MbTransferRate = dKbTransferRate / 1000.0;
			if (MbTransferRate > 1) {
				::swprintf(strTransferRate.GetBuffer(30), _T(" (%.1lf MB/sec)"), MbTransferRate);
				strTransferRate.ReleaseBuffer();
			} else {
				::swprintf(strTransferRate.GetBuffer(30), _T(" (%.1lf KB/sec)"), dKbTransferRate);
				strTransferRate.ReleaseBuffer();
			}

			// 残り 4 分
			int64_t nRestByte = data.fileSize - data.lastDLPos;
			if (dKbTransferRate > 0) {
				int nTotalSecondTime = static_cast<int>((nRestByte / 1000) / dKbTransferRate);	// 残り時間(秒)
				int nhourTime = nTotalSecondTime / (60 * 60);									// 時間
				int nMinTime = (nTotalSecondTime - (nhourTime * (60 * 60))) / 60;			// 分
				int nSecondTime = nTotalSecondTime - (nhourTime * (60 * 60)) - (nMinTime * 60);	// 秒
				data.detailText = _T("残り ");
				if (nhourTime > 0) {
					data.detailText += (boost::wformat(L"%1% 時間") % nhourTime).str().c_str();
				}
				if (nMinTime > 0) {
					data.detailText += (boost::wformat(L"%1% 分 ") % nMinTime).str().c_str();
				}
				data.detailText += (boost::wformat(L"%1% 秒 ― ") % nSecondTime).str().c_str();


				// 10.5 / 288 MB
				CString strDownloaded;
				double dMByte = (double)data.fileSize / (1000.0 * 1000.0);
				if (dMByte >= 1) {
					double DownloadMByte = (double)data.lastDLPos / (1000.0 * 1000.0);
					::swprintf(strDownloaded.GetBuffer(30), _T("%.1lf / %.1lf MB"), DownloadMByte, dMByte);
					strDownloaded.ReleaseBuffer();
				} else {
					double dKByte = (double)data.fileSize / 1000.0;
					double DownloadKByte = (double)data.lastDLPos / 1000.0;
					::swprintf(strDownloaded.GetBuffer(30), _T("%.1lf / %.1lf KB"), DownloadKByte, dKByte);
					strDownloaded.ReleaseBuffer();
				}
				data.detailText += strDownloaded + strTransferRate;
			}
		}

		for (auto& browserData : data.browserTransactionList) {
			auto funcCommaString = [](int64_t num) -> std::wstring {
				std::wstring numstr = std::to_wstring(num);
				std::wstring result;
				int count = 0;
				for (auto it = numstr.rbegin(); it != numstr.rend(); ++it) {
					++count;
					result.insert(result.begin(), *it);
					if (count < 3) {
						continue;

					} else {
						if (std::next(it) != numstr.rend()) {
							result.insert(result.begin(), L',');
						}
						count = 0;
					}
				}
				return result;
			};
			browserData.rangeText = (boost::wformat(L"[%1% - %2%]") 
									% funcCommaString(browserData.browserRangeBegin) 
									% funcCommaString(browserData.browserRangeEnd) ).str().c_str();
		}
		//InvalidateRect(_GetItemClientRect(it), FALSE);
	}
	Invalidate(FALSE);
}



void	CTransactionView::DoPaint(CDCHandle dc)
{
	CPoint ptOffset;
	GetScrollOffset(ptOffset);

	CRect rcClient;
	GetClientRect(rcClient);
	rcClient.MoveToY(ptOffset.y);
	//rcClient.bottom	+= ptOffset.y;

	CMemoryDC	memDC(dc, rcClient);
	HFONT hOldFont = memDC.SelectFont(m_Font);

	// 背景を描画
	memDC.FillSolidRect(rcClient, RGB(255, 255, 255));

	CCritSecLock lock(m_csTransDataList);
	for (auto it = m_transDataList.begin(); it != m_transDataList.end(); ++it) {
		auto& data = *it->get();

		CCritSecLock lock2(data.csData);

		CRect rcItem = data.rcItem;
		rcItem.right = rcClient.right;
		if (memDC.RectVisible(&rcItem) == FALSE)
			continue;

		memDC.SetBkMode(TRANSPARENT);
		/*if (it->dwState & DLITEMSTATE_SELECTED) {
			static COLORREF SelectColor = ::GetSysColor(COLOR_HIGHLIGHTTEXT);
			memDC.SetTextColor(SelectColor);
		} else*/ {
			memDC.SetTextColor(0);
		}

		// 選択列描画
		//if (it->dwState & DLITEMSTATE_SELECTED){
		//	memDC.FillRect(&rcItem, COLOR_HIGHLIGHT);
		//}

		// アイコン描画
		//CRect rcImage = rcItem;
		//CPoint ptImg(cxImageMargin, rcImage.top + cyImageMargin);
		//m_ImageList.Draw(memDC, data->nImgIndex, ptImg, ILD_NORMAL);

		// ファイル名を描画
		CRect rcFileName = rcItem;
		rcFileName.left = cxFileNameMargin;
		rcFileName.top += cyFileNameMargin;
		memDC.DrawText(data.name.c_str(), static_cast<int>(data.name.length()), rcFileName, DT_SINGLELINE);

		// progress
		CRect rcProgress;
		rcProgress.left = cxFileNameMargin;
		rcProgress.top = rcItem.top + cyProgressMargin;
		rcProgress.right = rcItem.right - cleftProgressMargin;
		rcProgress.bottom =  rcItem.top + cyProgressMargin + ProgressHeight;
		if (IsThemeNull() == false) {
			static int PROGRESSBAR = IsWindowsVistaOrGreater() ? PP_TRANSPARENTBAR : PP_BAR;
			static int PROGRESSBODY = IsWindowsVistaOrGreater() ? PP_FILL : PP_CHUNK;
			if (IsThemeBackgroundPartiallyTransparent(PROGRESSBAR, 0))
				DrawThemeParentBackground(memDC, rcProgress);
			DrawThemeBackground(memDC, PROGRESSBAR, 0, rcProgress);
			CRect rcContent;
			GetThemeBackgroundContentRect(memDC, PROGRESSBAR, 0, rcProgress, rcContent);

			double Propotion = double(data.lastDLPos) / double(data.fileSize);
			int nPos = (int)((double)rcContent.Width() * Propotion);
			rcContent.right = rcContent.left + nPos;
			DrawThemeBackground(memDC, PROGRESSBODY, 0, rcContent);
		} else {
			memDC.DrawEdge(rcProgress, EDGE_RAISED, BF_ADJUST | BF_MONO | BF_RECT | BF_MIDDLE);
			double Propotion = double(data.lastDLPos) / double(data.fileSize);
			int nPos = (int)((double)rcProgress.Width() * Propotion);
			rcProgress.right = rcProgress.left + nPos;
			memDC.FillSolidRect(rcProgress, RGB(0, 255, 0));
		}

		// 説明描画
		CRect rcDiscribe = rcItem;
		rcDiscribe.top += cyProgressMargin + ProgressHeight;
		rcDiscribe.left = cxFileNameMargin;
		memDC.DrawText(data.detailText, data.detailText.GetLength(), rcDiscribe, DT_SINGLELINE);

		// 停止アイコン描画
		//CPoint ptStop(rcClient.right - cxStopLeftSpace, rcItem.top + cyStopTopMargin);
		//m_ImgStop.Draw(memDC, 0, ptStop, ILD_NORMAL);

		// 下にラインを引く
		static COLORREF BorderColor = ::GetSysColor(COLOR_3DLIGHT);
		HPEN hPen = ::CreatePen(PS_SOLID, 1, BorderColor);
		HPEN hOldPen = memDC.SelectPen(hPen);
		memDC.MoveTo(CPoint(rcItem.left, rcItem.bottom));
		memDC.LineTo(rcItem.right, rcItem.bottom);
		memDC.SelectPen(hOldPen);
		::DeleteObject(hPen);

		int nBlackLineBottom = rcItem.bottom;

		for (auto& browserData : data.browserTransactionList) {
			CRect rcItem = browserData.rcItem;

			// 範囲テキスト
			CRect rcRangeText;
			rcRangeText.top = rcItem.top + cyBrowserTextTopMargin;
			rcRangeText.left = cxBrowserTextLeftMargin;
			rcRangeText.right = rcRangeText.left + cxBrowserTextWidth;
			rcRangeText.bottom = rcItem.bottom;
			memDC.DrawText(browserData.rangeText, browserData.rangeText.GetLength(), rcRangeText, DT_SINGLELINE);

			CRect rcProgress;
			rcProgress.left = cxBrowserLeftMargin;
			rcProgress.top = rcItem.top + cyBrowserProgressMargin;
			rcProgress.right = rcItem.right - cleftProgressMargin;
			rcProgress.bottom = rcItem.top + cyBrowserProgressMargin + ProgressHeight;
			if (IsThemeNull() == false) {
				static int PROGRESSBAR = IsWindowsVistaOrGreater() ? PP_TRANSPARENTBAR : PP_BAR;
				static int PROGRESSBODY = IsWindowsVistaOrGreater() ? PP_FILL : PP_CHUNK;
				if (IsThemeBackgroundPartiallyTransparent(PROGRESSBAR, 0))
					DrawThemeParentBackground(memDC, rcProgress);
				DrawThemeBackground(memDC, PROGRESSBAR, 0, rcProgress);
				CRect rcContent;
				GetThemeBackgroundContentRect(memDC, PROGRESSBAR, 0, rcProgress, rcContent);

				int64_t contentSize = browserData.browserRangeEnd - browserData.browserRangeBegin + 1;
				double Propotion = double(browserData.rangeBufferPos - browserData.browserRangeBegin) / double(contentSize);
				int nPos = (int)((double)rcContent.Width() * Propotion);
				rcContent.right = rcContent.left + nPos;
				DrawThemeBackground(memDC, PROGRESSBODY, 0, rcContent);
			} else {
				memDC.DrawEdge(rcProgress, EDGE_RAISED, BF_ADJUST | BF_MONO | BF_RECT | BF_MIDDLE);
				int64_t contentSize = browserData.browserRangeEnd - browserData.browserRangeBegin + 1;
				double Propotion = double(browserData.rangeBufferPos - browserData.browserRangeBegin) / double(contentSize);
				int nPos = (int)((double)rcProgress.Width() * Propotion);
				rcProgress.right = rcProgress.left + nPos;
				memDC.FillSolidRect(rcProgress, RGB(0, 255, 0));
			}

			// 下にラインを引く
			static COLORREF BorderColor = ::GetSysColor(COLOR_3DLIGHT);
			HPEN hPen = ::CreatePen(PS_SOLID, 1, BorderColor);
			HPEN hOldPen = memDC.SelectPen(hPen);
			memDC.MoveTo(CPoint(rcItem.left, rcItem.bottom));
			memDC.LineTo(rcItem.right, rcItem.bottom);
			memDC.SelectPen(hOldPen);
			::DeleteObject(hPen);

			nBlackLineBottom = rcItem.bottom;
		}

		// 下にラインを引く
		HPEN hBlackPen = ::CreatePen(PS_SOLID, 1, RGB(0, 0, 0));
		HPEN hBlackOldPen = memDC.SelectPen(hBlackPen);
		memDC.MoveTo(CPoint(0, nBlackLineBottom));
		memDC.LineTo(rcClient.right, nBlackLineBottom);
		memDC.SelectPen(hBlackOldPen);
		::DeleteObject(hBlackPen);
	}

	dc.SelectFont(hOldFont);
}




int CTransactionView::OnCreate(LPCREATESTRUCT lpCreateStruct)
{
	// 水平スクロールバーを削除
	ModifyStyle(WS_HSCROLL, 0);

	SetScrollSize(10, 10);
	SetScrollLine(10, 10);
	//SetScrollPage(100, 100);

	//// イメージリスト作成
	//m_ImageList.Create(32, 32, ILC_COLOR32 | ILC_MASK, 20, 100);

	// デフォルトのアイコンを読み込み
	//SHFILEINFO sfinfo;
	//::SHGetFileInfo(_T("*.*"), FILE_ATTRIBUTE_NORMAL, &sfinfo, sizeof(SHFILEINFO)
	//	, SHGFI_ICON | SHGFI_USEFILEATTRIBUTES);
	//int nImgIndex = m_ImageList.AddIcon(sfinfo.hIcon);	/* 0 */
	//::DestroyIcon(sfinfo.hIcon);

	//// 停止アイコンを読み込む
	//m_ImgStop.Create(16, 16, ILC_COLOR32 | ILC_MASK, 1, 1);
	//HICON hStopIcon = AtlLoadIcon(IDI_DLSTOP);
	//m_ImgStop.AddIcon(hStopIcon);
	//::DestroyIcon(hStopIcon);

	// フォントを設定
	WTL::CLogFont	logfont;
	logfont.SetMenuFont();
	m_Font = logfont.CreateFontIndirectW();

	//// ツールチップを設定
	//m_ToolTip.Create(m_hWnd);
	//m_ToolTip.ModifyStyle(0, TTS_ALWAYSTIP);
	//CToolInfo ti(TTF_SUBCLASS, m_hWnd);
	//ti.hwnd = m_hWnd;
	//m_ToolTip.AddTool(ti);
	//m_ToolTip.Activate(TRUE);
	//m_ToolTip.SetDelayTime(TTDT_AUTOPOP, 30 * 1000);
	//m_ToolTip.SetMaxTipWidth(600);

	OpenThemeData(L"PROGRESS");

	//RegisterDragDrop();

	return 0;
}

void CTransactionView::OnDestroy()
{
	SetMsgHandled(FALSE);

	KillTimer(kUpdateListTimerId);

}


void CTransactionView::OnSize(UINT nType, CSize size)
{
	SetMsgHandled(FALSE);

	if (size != CSize(0, 0)) {
		CCritSecLock lock(m_csTransDataList);
		for (auto& data : m_transDataList) {
			CCritSecLock lock2(data->csData);
			data->rcItem.right = size.cx;
			for (auto& browserData : data->browserTransactionList) {
				browserData.rcItem.right = size.cx;
			}
		}
		CSize	scrollsize;
		GetScrollSize(scrollsize);
		scrollsize.cx = size.cx ? size.cx : 1;
		m_sizeClient.cx = scrollsize.cx;
		SetScrollSize(scrollsize, FALSE, false);
		SetScrollLine(10, 10);
	}
}

void	CTransactionView::OnTimer(UINT_PTR nIDEvent)
{
	if (nIDEvent == kUpdateListTimerId) {
		_UpdateListInfo();
	}
#if 0
	for (auto it = m_vecTransData.begin(); it != m_vecTransData.end(); ++it) {
		if (reinterpret_cast<UINT_PTR>(it->get()) == nIDEvent) {
			m_vecTransData.erase(it);
			_RefreshList();
			KillTimer(nIDEvent);
			break;
		}
	}
#endif
}

/// nIndexのアイテムのクライアント座標での範囲を返す
CRect	CTransactionView::_GetItemClientRect(std::list<std::shared_ptr<TransactionData>>::iterator it)
{
	CPoint	ptOffset;
	GetScrollOffset(ptOffset);

	CRect rcItem = it->get()->rcItem;
	rcItem.top -= ptOffset.y;
	rcItem.bottom -= ptOffset.y;
	return rcItem;
}



//////////////////////////////////////////////////////////////////////
// CNicoConnectionFrame

std::shared_ptr<TransactionData>	CNicoConnectionFrame::CreateTransactionData(const std::wstring& name)
{
	return m_transactionView.CreateTransactionData(name);
}

void	CNicoConnectionFrame::DestroyTransactionData(std::shared_ptr<TransactionData> transData)
{
	m_transactionView.DestroyTransactionData(transData);
}



int		CNicoConnectionFrame::OnCreate(LPCREATESTRUCT lpCreateStruct)
{
	m_transactionView.Create(m_hWnd, 0, _T("TransactionView"), WS_CHILD | WS_VISIBLE);
	m_hWndClient = m_transactionView.m_hWnd;

	MoveWindow(100, 100, 500, 200);

	std::ifstream fs(Misc::GetExeDirectory() + _T("settings.ini"));
	if (fs) {
		try {
			ptree pt;
			read_ini(fs, pt);

			CRect rcWindow;
			rcWindow.top = pt.get("NicoConnectionFrame.top", 0);
			rcWindow.left = pt.get("NicoConnectionFrame.left", 0);
			rcWindow.right = pt.get("NicoConnectionFrame.right", 0);
			rcWindow.bottom = pt.get("NicoConnectionFrame.bottom", 0);
			if (rcWindow != CRect()) {
				MoveWindow(&rcWindow);
			}
			ShowWindow(pt.get("NicoConnectionFrame.ShowWindow", true));
		}
		catch (...) {
			MessageBox(GetTranslateMessage(ID_LOADSETTINGFAILED).c_str(), GetTranslateMessage(ID_TRANS_ERROR).c_str(), MB_ICONERROR);
			fs.close();
			MoveFile(Misc::GetExeDirectory() + _T("settings.ini"), Misc::GetExeDirectory() + _T("settings_loadfailed.ini"));
		}
	}
	return 0;
}

void	CNicoConnectionFrame::OnDestroy()
{
	std::string settingsPath = CT2A(Misc::GetExeDirectory() + _T("settings.ini"));
	ptree pt;
	try {
		read_ini(settingsPath, pt);
	}
	catch (...) {
		ERROR_LOG << L"CNicoConnectionFrame::OnDestroy : settings.iniの読み込みに失敗";
		pt.clear();
	}
	pt.put("NicoConnectionFrame.ShowWindow", m_bVisibleOnDestroy);
	if (m_bVisibleOnDestroy) {
		RECT rc;
		GetWindowRect(&rc);
		pt.put("NicoConnectionFrame.top", rc.top);
		pt.put("NicoConnectionFrame.left", rc.left);
		pt.put("NicoConnectionFrame.right", rc.right);
		pt.put("NicoConnectionFrame.bottom", rc.bottom);
	}

	write_ini(settingsPath, pt);
}

void	CNicoConnectionFrame::OnClose()
{
	m_bVisibleOnDestroy = IsWindowVisible() != 0;

}













