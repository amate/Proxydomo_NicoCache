/**
*	@file	NicoCacheManager.h
*/

#pragma once

#include <memory>
#include <string>
#include <unordered_map>
#include <thread>
#include <list>
#include <vector>
#include <atomic>
#include <boost\multi_index_container.hpp>
#include <boost\multi_index\sequenced_index.hpp>
#include <boost\multi_index\hashed_index.hpp>
#include <boost\multi_index\identity.hpp>
#include <boost\multi_index\member.hpp>
#include <boost\multi_index\mem_fun.hpp>
#include <boost\optional.hpp>
#include <atlsync.h>
#include "proximodo\url.h"
#include "FilterOwner.h"
#include "Socket.h"
#include "TransactionView.h"
#include "NicoMovieCacheManager.h"

using namespace boost::multi_index;


struct DLedNicoCache
{
	std::wstring name;
	std::string smNumber;
	std::wstring description;

	DLedNicoCache(const std::wstring& name, const std::string& smNumber, const std::wstring& description) :
		name(name), smNumber(smNumber), description(description)
	{}
};

struct NicoCommentList
{
	HeadPairList inHeaders;
	std::string commentListBody;

	HeadPairList inOwnerHeaders;
	std::string commentListOwnerBody;
};

struct VideoConvertItem
{
	std::wstring name;
	std::wstring progress;

	CCriticalSection csData;

	VideoConvertItem(const std::wstring& name) : name(name), progress(L"0%")
	{}
};

boost::optional<std::pair<int, int>>	GetDLCountAndClientDLCompleteCount(const std::string& smNumber);
void	DownloadThumbDataWhereIsNULL();


//////////////////////////////////////////////////////////////
// CNicoCacheManager

class CNicoCacheManager
{
	friend class CNicoMovieCacheManager;

	enum { kMaxParallelDLCount = 2 };
public:
	static void CreateNicoConnectionFrame();

	static void CloseAllConnection();

#pragma region associate smNumber => movieURL, title, thumbURL
	static bool IsGetFlvURL(const CUrl& url);
	static void TrapGetFlv(CFilterOwner& filterOwner, CSocket* sockBrowser);
	static void Associate_movieURL_smNumber(const std::string& movieURL, const std::string& smNumber);

	static bool IsMovieURL(const CUrl& url);
	static void ManageMovieCache(CFilterOwner& filterOwner, std::unique_ptr<CSocket>& sockBrowser);

	static void Associate_smNumberTitle(const std::string& smNumber, const std::wstring& title);
	static std::wstring Get_smNumberTitle(const std::string& smNumber);
	static std::string Get_smNumberMovieURL(const std::string& smNumber);

	static void Associate_smNumberThumbURL(const std::string& smNumber, const std::string& thumbURL);
	static std::string Get_smNumberThumbURL(const std::string& smNumber);
#pragma endregion
	static std::wstring FailFound(const std::string& smNumber);


	static void AddDLQue(const std::string& smNumber, const NicoRequestData& nicoRequestData);
	static void ConsumeDLQue();

	static void QueDownloadVideo(const std::wstring& watchPageURL);

	// CNicoMovieCacheManager が利用する
	static std::shared_ptr<TransactionData>	CreateTransactionData(const std::wstring& name);
	static void	DestroyTransactionData(std::shared_ptr<TransactionData> transData);

	static bool IsThumbURL(const CUrl& url);
	static void ManageThumbCache(CFilterOwner& filterOwner, std::unique_ptr<CSocket>& sockBrowser);

	static void AddDLedNicoCacheManager(const std::string& smNumber, std::shared_ptr<TransactionData> transData);

	// http://local.ptron/nicocache/nicocache_api へのリクエストを処理する
	static bool IsNicoCacheServerRequest(const CUrl& url);
	static void ManageNicoCacheServer(CFilterOwner& filterOwner, std::unique_ptr<CSocket>& sockBrowser);

	static bool	VideoConveter(const std::string& smNumber, const std::wstring& filePath, bool bForceConvert = false);

	static bool ManagePostCache(CFilterOwner& filterOwner, std::unique_ptr<CSocket>& sockBrowser, std::string& recvOutBuf);

private:

	static std::unique_ptr<CNicoConnectionFrame>	s_nicoConnectionFrame;

	struct MovieChunk {
		std::string smNumber;
		std::string movieURL;
		std::wstring title;

		MovieChunk(const std::string& smNumber, const std::string& movieURL, const std::wstring& title = L"") : 
			smNumber(smNumber), movieURL(movieURL), title(title)
		{}
	};

	struct smnum {};
	struct mvurl {};
	typedef boost::multi_index_container <
		MovieChunk,
		indexed_by<
			hashed_unique<tag<smnum>, member<MovieChunk, std::string, &MovieChunk::smNumber>>,	// set
			hashed_unique<tag<mvurl>, member<MovieChunk, std::string, &MovieChunk::movieURL>>	// set
		>
		> MovieChunkContainer;

	static CCriticalSection		s_csMovieChunkList;
	static MovieChunkContainer	s_movieChunkList;


	struct seq {}; // 挿入順のタグ
	struct hash {};
	typedef boost::multi_index_container <
		std::pair<std::string, std::shared_ptr<CNicoMovieCacheManager>>,
		indexed_by<
			sequenced<tag<seq>>,	// 挿入順
			hashed_unique<tag<hash>, 
			member<std::pair<std::string, std::shared_ptr<CNicoMovieCacheManager>>, std::string,
							&std::pair<std::string, std::shared_ptr<CNicoMovieCacheManager>>::first>>	// set
		>
	> ManagerContainer;

	static CCriticalSection	s_cssmNumberCacheManager;
	static ManagerContainer	s_mapsmNumberCacheManager;

	typedef boost::multi_index_container <
		std::pair<std::string, NicoRequestData>,
		indexed_by<
			sequenced<tag<seq>>,	// 挿入順
			hashed_unique<tag<hash>, member<std::pair<std::string, NicoRequestData>, std::string,
											&std::pair<std::string, NicoRequestData>::first>>	// set
		>
		> QueContainer;

	static CCriticalSection	s_cssmNumberDLQue;
	static QueContainer	s_smNumberDLQue;

	static CCriticalSection s_csvecDLedNicoCacheManager;
	static std::list<DLedNicoCache> s_vecDLedNicoCacheManager;

	static CCriticalSection s_csvideoConvertList;
	static std::list<VideoConvertItem> s_videoConvertList;

	static CCriticalSection s_cs_smNumberThumbURL;
	static std::unordered_map<std::string, std::string>	s_map_smNumberThumbURL;


	static CCriticalSection s_csCacheGetThumbInfo;
	static std::pair<std::string, std::string> s_cacheGetThumbInfo;
	
	static CCriticalSection s_csCacheWatchPage;
	static std::pair<std::string, std::string> s_cacheWatchPage;

	static CCriticalSection s_csCacheGetflv;
	static std::pair<std::string, std::string> s_cacheGetflv;

	static CCriticalSection s_csCacheCommentList;
	static std::pair<std::string, NicoCommentList> s_cacheCommentList;

	static CCriticalSection s_csLastOutHeaders;
	static HeadPairList s_lastOutHeaders;
};














