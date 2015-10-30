
#include "stdafx.h"
#include "NicoMovieCacheManager.h"
#include <regex>
#include <fstream>
#include <chrono>
#include <boost\lexical_cast.hpp>
#include "NicoCacheManager.h"
#include "Logger.h"
#include "Misc.h"
#include "CodeConvert.h"
#include "HttpOperate.h"
#include "NicoDatabase.h"
#include "NicoCacheMisc.h"

using namespace std::chrono;
using namespace CodeConvert;

#define CR	'\r'
#define LF	'\n'
#define CRLF "\r\n"

namespace {


}	// namespace

///////////////////////////////////////////////////
// CNicoMovieCacheManager

void CNicoMovieCacheManager::StartThread(NicoMoviewCacheStartData& startData)
{
	INFO_LOG << L"CNicoMovieCacheManager::StartThread : " << startData.smNumber;
	auto manager = std::shared_ptr<CNicoMovieCacheManager>(new CNicoMovieCacheManager);
	manager->m_active = true;
	manager->m_smNumber = startData.smNumber;

	manager->_CreateTransactionData(startData.smNumber);

	if (startData.optNicoRequestData) {
		manager->m_optNicoRequestData = startData.optNicoRequestData;
	} else {
		manager->NewBrowserConnection(startData.filterOwner, std::move(startData.sockBrowser));
	}
	//CCritSecLock lock2(CNicoCacheManager::s_cssmNumberCacheManager);
	auto result = CNicoCacheManager::s_mapsmNumberCacheManager.emplace_front(std::make_pair(startData.smNumber, manager));
	ATLASSERT(result.second);
	manager->m_thisThread = std::thread([manager]() {

		try {
			manager->Manage();
		}
		catch (std::exception& e) {
			ERROR_LOG << L"CNicoMovieCacheManager::StartThread : " << e.what();
			CNicoCacheManager::DestroyTransactionData(manager->m_transactionData);
		}

		CCritSecLock lock(CNicoCacheManager::s_cssmNumberCacheManager);
		if (manager->m_thisThread.joinable()) {
			manager->m_thisThread.detach();

			auto& mapList = CNicoCacheManager::s_mapsmNumberCacheManager.get<CNicoCacheManager::hash>();
			auto it = mapList.find(manager->m_smNumber);
			if (it == mapList.end()) {
				ATLASSERT(FALSE);
				return;
			}
			mapList.erase(it);
		}
	});
}

void	CNicoMovieCacheManager::ForceDatach()
{
	//CCritSecLock lock(CNicoCacheManager::s_cssmNumberCacheManager);
	SwitchToInvalid();
	m_thisThread.detach();

	auto& mapList = CNicoCacheManager::s_mapsmNumberCacheManager.get<CNicoCacheManager::hash>();
	auto it = mapList.find(m_smNumber);
	if (it == mapList.end()) {
		ATLASSERT(FALSE);
		return;
	}
	mapList.erase(it);
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
		transName = (LPCWSTR)Misc::GetFileBaseNoExt(cacheFileManager.LoadCachePath().c_str());
	} else {
		transName = CreateCacheFilePath(smNumber, bLowRequest);
		auto slashPos = transName.rfind(L'\\');
		ATLASSERT(slashPos != std::wstring::npos);
		transName = transName.substr(slashPos + 1);
	}
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
	//INFO_LOG << L"_InitRangeSetting : " << browserRangeRequest.GetID();

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
	WriteSocketBuffer(browserRangeRequest.sockBrowser.get(), sendInBuf.c_str(), static_cast<int>(sendInBuf.length()));
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
	if (CNicoDatabase::GetInstance().AddNicoHistory(m_smNumber, UTF8fromUTF16(title))) {
		std::string thumbURL = CNicoCacheManager::Get_smNumberThumbURL(m_smNumber);
		if (thumbURL.length() > 0) {
			std::string thumbData = GetThumbData(thumbURL + ".M");
			if (thumbData.empty()) {
				thumbData = GetThumbData(thumbURL);
			}
			if (thumbData.length() > 0) {
				CNicoDatabase::GetInstance().SetThumbData(m_smNumber, thumbData.data(), static_cast<int>(thumbData.size()));
			} else {
				ERROR_LOG << m_smNumber << L" no thumbData";
				ATLASSERT(FALSE);
			}
		} else {
			ERROR_LOG << m_smNumber << L" no thumbURL";
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
				WriteSocketBuffer(m_browserRangeRequestList.front().sockBrowser.get(), sendInBuf.c_str(), static_cast<int>(sendInBuf.length()));
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
		m_transactionData->oldDLPos = m_movieCacheBuffer.size();

		fs.write(buffer.c_str(), buffer.size());
		m_movieCacheBuffer += buffer;
		m_lastMovieSize = m_movieCacheBuffer.size();
		m_transactionData->lastDLPos = m_lastMovieSize;

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
	boost::optional<steady_clock::time_point> optboostTimeCountStart;
	// クライアントダウンロード時のみboostを掛ける
	if (m_browserRangeRequestList.size() > 0) {
		optboostTimeCountStart = steady_clock::now();
	}
	int	boostCount = 0;
	enum { kMaxBoostCount = 2 };
	for (;;) {
		{
			CCritSecLock lock(m_csBrowserRangeRequestList);
			for (auto& browserRangeRequest : m_browserRangeRequestList) {
				if (browserRangeRequest.bSendResponseHeader)
					continue;

				_InitRangeSetting(browserRangeRequest);
				_SendResponseHeader(browserRangeRequest);

				lastTime = steady_clock::time_point();
				//INFO_LOG << browserRangeRequest.GetID() << L" browser request";
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
					ERROR_LOG << m_smNumber << L" サイトとの接続が切れたが、ファイルをすべてDLできませんでした...";

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
		int64_t bufferSize = m_movieCacheBuffer.size();

		{
			CCritSecLock lock(m_csBrowserRangeRequestList);
			for (auto& browserRangeRequest : m_browserRangeRequestList) {
				if (browserRangeRequest.bSendResponseHeader == false)
					continue;

				if (browserRangeRequest.sockBrowser->IsConnected() == false) {
					//INFO_LOG << browserRangeRequest.GetID() << L" browser disconnection";
					suicideList.emplace_back(browserRangeRequest.itThis);

				} else {
					if (browserRangeRequest.rangeBufferPos < bufferSize) {
						int64_t restRangeSize = browserRangeRequest.browserRangeEnd - browserRangeRequest.rangeBufferPos + 1;
						int64_t sendSize = bufferSize - browserRangeRequest.rangeBufferPos;
						if (restRangeSize < sendSize)
							sendSize = restRangeSize;

						//INFO_LOG << browserRangeRequest.GetID() << L" RangeBegin : " << browserRangeRequest.browserRangeBegin << L" RangeEnd : " << browserRangeRequest.browserRangeEnd << L" RangeBufferPos : " << browserRangeRequest.rangeBufferPos << L" sendSize : " << sendSize;

						WriteSocketBuffer(browserRangeRequest.sockBrowser.get(),
							m_movieCacheBuffer.c_str() + browserRangeRequest.rangeBufferPos, static_cast<int>(sendSize));
						browserRangeRequest.rangeBufferPos += sendSize;

						auto browserData = m_transactionData->GetBrowserTransactionData(static_cast<void*>(&browserRangeRequest));
						browserData->rangeBufferPos = browserRangeRequest.rangeBufferPos;

						if (browserRangeRequest.rangeBufferPos > browserRangeRequest.browserRangeEnd) {	// 終わり
																										//INFO_LOG << browserRangeRequest.GetID() << L" browser close";
							browserRangeRequest.sockBrowser->Close();
							suicideList.emplace_back(browserRangeRequest.itThis);

							if (browserRangeRequest.rangeBufferPos == m_movieSize && clientDLComplete == false) {
								CNicoDatabase::GetInstance().ClientDLComplete(m_smNumber);
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
					if ((steady_clock::now() - lastTime) > minutes(3)) {
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
			++boostCount;
			if (boostCount <= kMaxBoostCount) {
				INFO_LOG << m_smNumber << L" boost retry download";
				_RetryDownload(nicoRequestData, fs);
				optboostTimeCountStart = steady_clock::now();

			} else {
				optboostTimeCountStart.reset();	// boost stop
			}
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








