#pragma once

#include <memory>
#include <list>
#include <thread>
#include <atlsync.h>
#include <atomic>
#include <string>
#include <boost\optional.hpp>
#include "Socket.h"
#include "FilterOwner.h"
#include "TransactionView.h"


struct NicoRequestData
{
	CUrl	url;
	HeadPairList outHeaders;
};

struct NicoMoviewCacheStartData
{
	std::string	smNumber;
	CFilterOwner filterOwner; 
	std::unique_ptr<CSocket> sockBrowser;

	NicoMoviewCacheStartData(const std::string& smNumber, const CFilterOwner& filterOwner, std::unique_ptr<CSocket>&& sockBrowser) :
		smNumber(smNumber), filterOwner(filterOwner), sockBrowser(std::move(sockBrowser))
	{}

	boost::optional<NicoRequestData>	optNicoRequestData;

	NicoMoviewCacheStartData(const std::string& smNumber, const NicoRequestData& nicoRequestData) : 
		smNumber(smNumber), optNicoRequestData(nicoRequestData)
	{}
};

class CNicoCacheManager;

//////////////////////////////////////////////////////////////
// CNicoMovieCacheManager

class CNicoMovieCacheManager
{
	friend class CNicoCacheManager;

	struct BrowserRangeRequest {
		bool			bSendResponseHeader;
		CFilterOwner	filterOwner;
		std::unique_ptr<CSocket>	sockBrowser;

		int64_t	browserRangeBegin;
		int64_t browserRangeEnd;
		int64_t browserRangeLength;

		int64_t rangeBufferPos;

		std::list<BrowserRangeRequest>::iterator itThis;

		BrowserRangeRequest() : bSendResponseHeader(false),
			browserRangeBegin(0), browserRangeEnd(0), browserRangeLength(0), rangeBufferPos(0)
		{}

		std::wstring GetID()
		{
			std::wstring range = CFilterOwner::GetHeader(filterOwner.outHeaders, L"Range");
			return L"[" + range + L"]";
		}
	};

	CNicoMovieCacheManager() : m_bReserveVideoConvert(false), m_retryCount(0)
	{}

public:

	static void StartThread(NicoMoviewCacheStartData& startData);

	void	NewBrowserConnection(CFilterOwner& filterOwner, std::unique_ptr<CSocket>&& sockBrowser);

	void	ReserveVideoConvert() {
		// DL’†‚¶‚á‚È‚«‚á—LŒø‚É‚µ‚Ä‚àˆÓ–¡‚ª‚È‚¢
		ATLASSERT(m_movieSize > 0 && m_movieSize != m_movieCacheBuffer.size());
		m_bReserveVideoConvert = true;
	}

	std::string smNumber() const { return m_smNumber; }

	bool	IsValid() const { return m_active; }
	void	SwitchToInvalid();
	void	ForceStop();
	void	ForceDatach();

	void	Manage();

private:
	void	_CreateTransactionData(const std::string& smNumber);

	void	_InitRangeSetting(BrowserRangeRequest& browserRangeRequest);
	void	_SendResponseHeader(BrowserRangeRequest& browserRangeRequest);

	bool	_RetryDownload(const NicoRequestData& nicoRequestData, std::ofstream& fs);

	std::atomic_bool	m_active;
	std::string m_smNumber;
	std::thread	m_thisThread;

	boost::optional<NicoRequestData>	m_optNicoRequestData;

	std::unique_ptr<CSocket>	m_sockWebsite;

	int64_t			m_movieSize;
	std::string		m_movieCacheBuffer;
	int64_t			m_lastMovieSize;

	CCriticalSection	m_csBrowserRangeRequestList;
	std::list<BrowserRangeRequest>	m_browserRangeRequestList;

	std::shared_ptr<TransactionData>	m_transactionData;

	bool	m_bReserveVideoConvert;

	enum { kMaxRetryCount = 10 };
	int		m_retryCount;

};