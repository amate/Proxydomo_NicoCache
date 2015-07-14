/**
*	@file	TransactionView.h
*/

#pragma once

#include <cstdint>
#include <vector>
#include <list>
#include <map>
#include <deque>
#include <memory>
#include <functional>
#include <atomic>
#include <atlapp.h>
#include <atlframe.h>
#include <atlscrl.h>
#include <atlcrack.h>
#define _WTL_USE_VSSYM32
#include <atltheme.h>
#include <atlsync.h>
#include <atlctrls.h>
#define _WTL_NO_CSTRING
#include <atlmisc.h>

struct TransactionData
{
	std::atomic<int64_t> fileSize;
	std::atomic<int64_t> lastDLPos;
	int64_t	oldDLPos;
	std::wstring name;

	CString	detailText;
	std::deque<std::pair<int64_t, DWORD>> deqProgressAndTime;
	CRect	rcItem;
	std::list<std::shared_ptr<TransactionData>>::iterator itThis;
	CCriticalSection	csData;

	TransactionData(const std::wstring& name) : fileSize(0), lastDLPos(0), oldDLPos(0), name(name)
	{}

	struct BrowserTransationData
	{
		std::atomic<int64_t> browserRangeBegin;
		std::atomic<int64_t> browserRangeEnd;

		std::atomic<int64_t> rangeBufferPos;

		CRect	rcItem;
		CString rangeText;
		void*	originalPointer;	// çÌèúópÇÃñ⁄àÛ

		BrowserTransationData(void* pointer) : 
			originalPointer(pointer), browserRangeBegin(0), browserRangeEnd(0)
		{}
	};
	std::list<BrowserTransationData> browserTransactionList;

	void	AddBrowserTransaction(void* pointer)
	{
		CCritSecLock lock(csData);
		browserTransactionList.emplace_front(pointer);
		lock.Unlock();

		funcRefreshList();
	}

	BrowserTransationData* GetBrowserTransactionData(void* pointer)
	{
		for (auto& browserData : browserTransactionList) {
			if (browserData.originalPointer == pointer) {
				return &browserData;
			}
		}
		ATLASSERT(FALSE);
		return nullptr;
	}

	void	RemoveBrowserTransaction(void* pointer)
	{
		CCritSecLock lock(csData);
		for (auto it = browserTransactionList.begin(); it != browserTransactionList.end(); ++it) {
			if (it->originalPointer == pointer) {
				browserTransactionList.erase(it);
				lock.Unlock();

				funcRefreshList();
				return;
			}
		}
		ATLASSERT(FALSE);
	}

	std::function<void ()> funcRefreshList;

};

class CTransactionView : 
	public CScrollWindowImpl<CTransactionView>, 
	public CThemeImpl<CTransactionView>
{
public:

	// Constants
	enum {
		ItemHeight = 50,
		UnderLineHeight = 1,

		cxImageMargin = 3,
		cyImageMargin = (ItemHeight - 32) / 2,

		cxFileNameMargin = 5,//cxImageMargin + 32 + cxImageMargin,
		cyFileNameMargin = 3,

		ProgressHeight = 12,
		cyProgressMargin = 21,
		cleftProgressMargin = 5, //30,

		//cxStopLeftSpace = 23,
		//cyStopTopMargin = 19,
		//cxStop = 16,
		//cyStop = 16,
		cxBrowserTextLeftMargin = 5,
		cyBrowserTextTopMargin = 3,
		cxBrowserTextWidth = 150,
		cxBrowserLeftMargin = cxBrowserTextLeftMargin + cxBrowserTextWidth + 3,
		cyBrowserHeight = 20,
		cyBrowserProgressMargin = (cyBrowserHeight - ProgressHeight) / 2,

		TOOLTIPTIMERID = 2,
		TOOLTIPDELAY = 250,

		kRemoveTransactionTimerInterval = 3000,

		kUpdateListTimerId = 1,
		kUpdateListTimerInterval = 1000,
	};

	CTransactionView();
	~CTransactionView();

	std::shared_ptr<TransactionData>	CreateTransactionData(const std::wstring& name);
	void	DestroyTransactionData(std::shared_ptr<TransactionData> transData);

	// Overrides
	void	DoPaint(CDCHandle dc);

	// Message map
	BEGIN_MSG_MAP_EX(CTransactionView)
		MSG_WM_CREATE(OnCreate)
		MSG_WM_DESTROY(OnDestroy)
		MSG_WM_SIZE(OnSize)
		//MSG_WM_LBUTTONDOWN(OnLButtonDown)
		//MSG_WM_RBUTTONUP(OnRButtonUp)
		MSG_WM_ERASEBKGND(OnEraseBkgnd)
		MSG_WM_TIMER(OnTimer)
		//MSG_WM_COPYDATA(OnCopyData)

		CHAIN_MSG_MAP(CScrollWindowImpl<CTransactionView>)
		CHAIN_MSG_MAP_ALT(CScrollWindowImpl<CTransactionView>, 1)
		CHAIN_MSG_MAP(CThemeImpl<CTransactionView>)
	END_MSG_MAP()

	int		OnCreate(LPCREATESTRUCT lpCreateStruct);
	void	OnDestroy();
	void	OnSize(UINT nType, CSize size);
	//void	OnLButtonDown(UINT nFlags, CPoint point);
	//void	OnRButtonUp(UINT nFlags, CPoint point);
	BOOL	OnEraseBkgnd(CDCHandle dc) { return TRUE; }
	void	OnTimer(UINT_PTR nIDEvent);
	//BOOL	OnCopyData(CWindow wnd, PCOPYDATASTRUCT pCopyDataStruct);

private:
	void	_RefreshList();
	void	_UpdateListInfo();
	CRect	_GetItemClientRect(std::list<std::shared_ptr<TransactionData>>::iterator it);

	// Data members
	CCriticalSection	m_csTransDataList;
	std::list<std::shared_ptr<TransactionData>>	m_transDataList;

	DWORD m_dwLastTime;

	CFont	m_Font;
};


//////////////////////////////////////////////////////////////////////
// CNicoConnectionFrame

class CNicoConnectionFrame : public CFrameWindowImpl< CNicoConnectionFrame >
{
public:
	DECLARE_FRAME_WND_CLASS(_T("NicoConnectionFrame"), NULL)

	CNicoConnectionFrame() : m_bVisibleOnDestroy(true)
	{}

	std::shared_ptr<TransactionData>	CreateTransactionData(const std::wstring& name);
	void	DestroyTransactionData(std::shared_ptr<TransactionData> transData);


	// Message map and Handler
	BEGIN_MSG_MAP(CNicoConnectionFrame)
		MSG_WM_CREATE(OnCreate)
		MSG_WM_DESTROY(OnDestroy)
		MSG_WM_CLOSE(OnClose)
		CHAIN_MSG_MAP(CFrameWindowImpl<CNicoConnectionFrame>)
	END_MSG_MAP()


	int		OnCreate(LPCREATESTRUCT lpCreateStruct);
	void	OnDestroy();
	void	OnClose();

private:
	bool m_bVisibleOnDestroy;
	CTransactionView	m_transactionView;
};













