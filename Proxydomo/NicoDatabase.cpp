#include "stdafx.h"
#include "NicoDatabase.h"
#include <sqlite3.h>
#include <thread>
#include "Misc.h"
#include "Logger.h"
#include "CodeConvert.h"
using namespace CodeConvert;

#include <fstream>

namespace {

const char* kDatabaseName = "nicoDatabase.db";

class CSQLiteStatement
{
public:
	CSQLiteStatement(sqlite3* db, const std::string& sql) : m_stmt(nullptr)
	{
		int err = sqlite3_prepare_v2(db, sql.c_str(), static_cast<int>(sql.length()), &m_stmt, nullptr);
		if (err != SQLITE_OK) {
			throw std::runtime_error("sqlite3_prepare_v2 failed");
		}
	}

	~CSQLiteStatement()
	{
		sqlite3_finalize(m_stmt);
	}

	// Bindxxx の placePos は 1 から指定する
	void	BindText(int placePos, const std::string& text)
	{
		int err = sqlite3_bind_text(m_stmt, placePos, text.c_str(), static_cast<int>(text.length()), SQLITE_TRANSIENT);
		if (err != SQLITE_OK) {
			throw std::runtime_error("sqlite3_bind_text failed");
		}
	}

	void	BindBlob(int placePos, const char* data, int size)
	{
		int err = sqlite3_bind_blob(m_stmt, placePos, (const void*)data, size, SQLITE_TRANSIENT);
		if (err != SQLITE_OK) {
			throw std::runtime_error("sqlite3_bind_text failed");
		}
	}

	int		Step()
	{
		int err = sqlite3_step(m_stmt);
		return err;
	}

	// Columnxxx の iCol は 0 から指定する
	std::string		ColumnText(int iCol)
	{
		const char* text = (const char*)sqlite3_column_text(m_stmt, iCol);
		return text;
	}

	std::wstring	ColumnText16(int iCol)
	{
		const wchar_t* text = (const wchar_t*)sqlite3_column_text16(m_stmt, iCol);
		return text;
	}

	int		ColumnInt(int iCol)
	{
		int n = sqlite3_column_int(m_stmt, iCol);
		return n;
	}

	std::vector<char>	ColumnBlob(int iCol)
	{
		std::vector<char> vec;
		int bytes = sqlite3_column_bytes(m_stmt, iCol);
		if (bytes > 0) {
			const char* data = (const char*)sqlite3_column_blob(m_stmt, iCol);
			vec.resize(bytes);
			memcpy_s(vec.data(), bytes, data, bytes);
		}
		return vec;
	}

	bool	ColumnIsNull(int iCol)
	{
		bool isNull = sqlite3_column_bytes(m_stmt, iCol) == 0;
		return isNull;
	}

private:
	sqlite3_stmt* m_stmt;
};

}	// namespace

	// ファイル内容を読み込み
std::string LoadFile(const std::wstring& filePath)
{
	std::ifstream fscache(filePath, std::ios::in | std::ios::binary);
	if (!fscache)
		throw std::runtime_error("file open failed : " + std::string(CW2A(filePath.c_str())));

	std::string data;
	fscache.seekg(0, std::ios::end);
	auto fileSize = fscache.tellg();
	fscache.seekg(0, std::ios::beg);
	fscache.clear();
	data.resize(static_cast<size_t>(fileSize));
	fscache.read(const_cast<char*>(data.data()), static_cast<size_t>(fileSize));
	fscache.close();

	return data;
}

std::string	GetThumbData(const std::string& thumbURL);
std::string GetThumbURL(const std::string& smNumber);

CNicoDatabase::CNicoDatabase() : m_db(nullptr)
{
	CString dbPath =  Misc::GetExeDirectory() + kDatabaseName;
	int err = sqlite3_open((LPSTR)CT2A(dbPath), &m_db);
	if (err != SQLITE_OK) {
		ERROR_LOG << L"sqlite3_open failed";
		throw std::runtime_error("sqlite3_open failed");
	}

	char* errmsg = nullptr;
	err = sqlite3_exec(m_db,
					R"(CREATE TABLE IF NOT EXISTS nicoHistory(
						smNumber TEXT PRIMARY KEY, 
						title TEXT, 
						DLCount INTEGER DEFAULT 1, 
						ClientDLCompleteCount INTEGER DEFAULT 0, 
						firstAccessTime DEFAULT CURRENT_TIMESTAMP, 
						lastAccessTime DEFAULT CURRENT_TIMESTAMP,
						thumbData BLOB
						);)",
						nullptr, nullptr, &errmsg);
	if (err != SQLITE_OK) {
		ERROR_LOG << L"sqlite3_exec failed : " << errmsg;
		sqlite3_free(errmsg);
		throw std::runtime_error("sqlite3_exec failed");
	}

//	AddNicoHistory("sm9", UTF8fromUTF16(L"akuryou"));

#if 0
	std::string imageData = LoadFile(UTF16fromUTF8(R"(D:\Program\Proxydomo_NicoCache\#Release\Proxydomo64\nico_cache\thumb_cache\27457335.L.jpg)"));
	SetThumbData("sm27451746", imageData.data(), imageData.size());

	CSQLiteStatement stmt(m_db, "SELECT thumbData FROM nicoHistory WHERE smNumber = ?;");
	stmt.BindText(1, "sm27451746");

	err = stmt.Step();
	auto data = stmt.ColumnBlob(0);
#endif

	//auto list = QueryNicoHistoryList();

}

CNicoDatabase::~CNicoDatabase()
{
	int err = sqlite3_close(m_db);
	if (err != SQLITE_OK) {
		ERROR_LOG << L" sqlite3_close failed";
	}
}



bool	CNicoDatabase::AddNicoHistory(const std::string& smNumber, const std::string& title)
{
	CCritSecLock lock(m_cs);
	CSQLiteStatement stmt(m_db, "INSERT INTO nicoHistory(smNumber, title) VALUES(?, ?);");
	stmt.BindText(1, smNumber);
	stmt.BindText(2, title);

	int err = stmt.Step();
	if (err == SQLITE_DONE) {
		return true;

	} else if (err == SQLITE_CONSTRAINT) {
		CSQLiteStatement stmt2(m_db, "UPDATE nicoHistory SET title = ?, DLCount = DLCount + 1, lastAccessTime = CURRENT_TIMESTAMP WHERE smNumber = ?;");
		stmt2.BindText(1, title);
		stmt2.BindText(2, smNumber);

		err = stmt2.Step();
		ATLASSERT(err == SQLITE_DONE);
		return false;
	}
	ATLASSERT(FALSE);
	return false;
}

void	CNicoDatabase::ClientDLComplete(const std::string& smNumber)
{
	CCritSecLock lock(m_cs);
	CSQLiteStatement stmt(m_db, "UPDATE nicoHistory SET ClientDLCompleteCount = ClientDLCompleteCount + 1 WHERE smNumber = ?;");
	stmt.BindText(1, smNumber);

	int err = stmt.Step();
	ATLASSERT(err == SQLITE_DONE);
}

boost::optional<std::pair<int, int>>	CNicoDatabase::GetDLCountAndClientDLCompleteCount(const std::string& smNumber)
{
	CCritSecLock lock(m_cs);
	CSQLiteStatement stmt(m_db, "SELECT DLCount, ClientDLCompleteCount FROM nicoHistory WHERE smNumber = ?;");
	stmt.BindText(1, smNumber);

	int err = stmt.Step();
	if (err != SQLITE_ROW)
		return boost::none;

	int DLCount =  stmt.ColumnInt(0);
	int ClientDLCompleteCount = stmt.ColumnInt(1);
	return std::make_pair(DLCount, ClientDLCompleteCount);
}

void	CNicoDatabase::SetThumbData(const std::string& smNumber, const char* data, int size)
{
	ATLASSERT(size > 0);

	CCritSecLock lock(m_cs);
	CSQLiteStatement stmt(m_db, "UPDATE nicoHistory SET thumbData = ? WHERE smNumber = ?;");
	stmt.BindBlob(1, data, size);
	stmt.BindText(2, smNumber);

	int err = stmt.Step();
	ATLASSERT(err == SQLITE_DONE);
}


std::list<NicoHistory>	CNicoDatabase::QueryNicoHistoryList()
{
	std::list<NicoHistory> nicoHistoryList;

	CCritSecLock lock(m_cs);
	CSQLiteStatement stmt(m_db, 
		R"(SELECT smNumber, title, DLCount, ClientDLCompleteCount, lastAccessTime, thumbData 
			FROM nicoHistory ORDER BY ROWID DESC;)");
	int err;
	while ((err = stmt.Step()) == SQLITE_ROW) {
		std::string smNumber = stmt.ColumnText(0);
		std::string title = stmt.ColumnText(1);
		int DLCount = stmt.ColumnInt(2);
		int ClientDLComleteCount = stmt.ColumnInt(3);
		std::string lastAccessTime = stmt.ColumnText(4);
		std::vector<char> thumbData = stmt.ColumnBlob(5);
		nicoHistoryList.emplace_back(smNumber, title, DLCount, ClientDLComleteCount, lastAccessTime, std::move(thumbData));
	}
	ATLASSERT(err == SQLITE_DONE);
	return nicoHistoryList;
}

void	CNicoDatabase::DownloadThumbDataWhereIsNULL()
{
	CCritSecLock lock(m_cs);
	CSQLiteStatement stmt(m_db, "SELECT smNumber FROM nicoHistory WHERE thumbData IS NULL;");
	while (stmt.Step() == SQLITE_ROW) {
		std::string smNumber = stmt.ColumnText(0);
		std::string thumbURL = GetThumbURL(smNumber);
		if (thumbURL.length() > 0) {
			std::string thumbData = GetThumbData(thumbURL + ".M");
			if (thumbData.empty()) {
				thumbData = GetThumbData(thumbURL);
			}
			if (thumbData.size() > 0) {
				SetThumbData(smNumber, thumbData.data(), thumbData.size());
				INFO_LOG << L"DownloadThumbDataWhereIsNULL SetThumbData smNumber : " << smNumber;
			} else {
				ATLASSERT(FALSE);
				ERROR_LOG << L"DownloadThumbDataWhereIsNULL failed smNumber : " << smNumber;
			}
		}
	}
}

