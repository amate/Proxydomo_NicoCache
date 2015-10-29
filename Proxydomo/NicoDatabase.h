#pragma once

#include <string>
#include <vector>
#include <list>
#include <atlsync.h>
#include <boost\optional.hpp>

struct sqlite3;

struct NicoHistory
{
	std::string smNumber;
	std::string title;
	int		DLCount;
	int		ClientDLCompleteCount;
	std::string lastAccessTime;
	std::vector<char>	thumbData;

	NicoHistory(const std::string& smNumber, const std::string& title, int DLCount, int ClientDLCompleteCount, const std::string& lastAccessTime, std::vector<char>&&  thumbData) : 
		smNumber(smNumber), title(title), DLCount(DLCount), ClientDLCompleteCount(ClientDLCompleteCount), lastAccessTime(lastAccessTime), thumbData(std::move(thumbData))
	{}
};

enum class NicoListQuery
{
	kDownloadOrderDesc,
	kClientDLIncompleteOrderDesc,
	kLastAccessTimerOrderDesc,
};

class CNicoDatabase
{
public:
	CNicoDatabase();
	~CNicoDatabase();

	bool	AddNicoHistory(const std::string& smNumber, const std::string& title);
	void	ClientDLComplete(const std::string& smNumber);
	boost::optional<std::pair<int, int>>	GetDLCountAndClientDLCompleteCount(const std::string& smNumber);
	void	SetThumbData(const std::string& smNumber, const char* data, int size);

	std::list<NicoHistory>	QueryNicoHistoryList(NicoListQuery query);

	void	DownloadThumbDataWhereIsNULL();

private:
	sqlite3*	m_db;
	CCriticalSection	m_cs;
};

