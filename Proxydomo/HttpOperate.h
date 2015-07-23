/**
*
*/

#pragma once

#include <string>
#include <memory>
#include "Socket.h"
#include "FilterOwner.h"


// sock ����buffer�֓ǂݍ���
bool ReadSocketBuffer(CSocket* sock, std::string& buffer);

// sock �� buffer �� length ����������
void WriteSocketBuffer(CSocket* sock, const char* buffer, int length);

enum class HttpVerb {
	kHttpGet, kHttpPost,
};
// url �֐ڑ����� ���N�G�X�g�𑗐M����
std::unique_ptr<CSocket>	SendRequest(const CUrl& url, HttpVerb verb, HeadPairList& outHeaders);


// sockWebsite �Ɍq�����Ă���T�C�g����filterOwner.inHeaders �� ���X�|���X�w�b�_���擾����
void GetResponseHeader(CFilterOwner& filterOwner, CSocket* sockWebsite, std::string& buffer);

// sockWebsite �Ɍq�����Ă���T�C�g����filterOwner.inHeaders ���� Content-Length���_�E�����[�h���ĕԂ�
std::string GetResponseBody(CFilterOwner& filterOwner, CSocket* sockWebsite, std::string& buffer);


// filterOwner.url ����t�@�C�����_�E�����[�h����
std::string GetHTTPPage(CFilterOwner& filterOwner);

// sockBrowser �� filterOwner.inHeaders �� body ���𑗐M���āA���M�������e��Ԃ�
std::string SendResponse(CFilterOwner& filterOwner, CSocket* sockBrowser, const std::string& body);

// sockBrowser �� HTTP1.1 200 OK ���X�|���X�𑗐M����
void	SendHTTPOK_Response(CSocket* sockBrowser, HeadPairList& inHeaders, const std::string& body);













