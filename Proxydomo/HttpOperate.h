/**
*
*/

#pragma once

#include <string>
#include <memory>
#include "Socket.h"
#include "FilterOwner.h"


// sock からbufferへ読み込む
bool ReadSocketBuffer(CSocket* sock, std::string& buffer);

// sock へ buffer を length 分書き込む
void WriteSocketBuffer(CSocket* sock, const char* buffer, int length);

enum class HttpVerb {
	kHttpGet, kHttpPost,
};
// url へ接続して リクエストを送信する
std::unique_ptr<CSocket>	SendRequest(const CUrl& url, HttpVerb verb, HeadPairList& outHeaders);


// sockWebsite に繋がっているサイトからfilterOwner.inHeaders へ レスポンスヘッダを取得する
void GetResponseHeader(CFilterOwner& filterOwner, CSocket* sockWebsite, std::string& buffer);

// sockWebsite に繋がっているサイトからfilterOwner.inHeaders 内の Content-Length分ダウンロードして返す
std::string GetResponseBody(CFilterOwner& filterOwner, CSocket* sockWebsite, std::string& buffer);


// filterOwner.url からファイルをダウンロードする
std::string GetHTTPPage(CFilterOwner& filterOwner);

// sockBrowser へ filterOwner.inHeaders と body 分を送信して、送信した内容を返す
std::string SendResponse(CFilterOwner& filterOwner, CSocket* sockBrowser, const std::string& body);

// sockBrowser へ HTTP1.1 200 OK レスポンスを送信する
void	SendHTTPOK_Response(CSocket* sockBrowser, HeadPairList& inHeaders, const std::string& body);













