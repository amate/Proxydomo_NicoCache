/**
*
*/

#include "stdafx.h"
#include "MediaInfo.h"
#include "..\libmediainfo_0.7.75_AllInclusive\MediaInfoLib\Source\MediaInfoDLL\MediaInfoDLL.h"
#pragma comment(lib, "MediaInfo.lib")
using namespace MediaInfoDLL;

// MI.Get �̑�O������ 
// MediaInfoLib\Source\Resource\Text\Stream
// �t�H���_�ȉ��ɂ���csv�t�@�C���ɂǂ̃e�L�X�g�������΂�����������

std::unique_ptr<VideoInfo>	GetVideoInfo(const std::wstring& filePath)
{
	auto videoInfo = std::make_unique<VideoInfo>();

	MediaInfo MI;
	if (MI.Open(filePath) == 0)
		return nullptr;

	videoInfo->width = std::stoi(MI.Get(Stream_Video, 0, _T("Width")).c_str());
	videoInfo->height = std::stoi(MI.Get(Stream_Video, 0, _T("Height")).c_str());

	videoInfo->formatProfile = MI.Get(Stream_Video, 0, _T("Format_Profile"));

	videoInfo->ref_frames = std::stoi(MI.Get(Stream_Video, 0, _T("Format_Settings_RefFrames")));

	std::wstring encodeSettings = MI.Get(Stream_Video, 0, _T("Encoded_Library_Settings"));
	// weightp=2 ����iPhone4S�ŉf�����~�܂��Ă��܂��H (��: sm26702656, sm26701432(High@L5.0�Ȃ̂ōĐ��ł��Ȃ�)
	auto pos = encodeSettings.find(L"weightb=");
	if (pos != std::wstring::npos) {
		std::wstring weightb = encodeSettings.substr(pos + 8, 1);
		videoInfo->weightb = std::stoi(weightb);
	} else {
		videoInfo->weightb = 0;
	}

	return videoInfo;
}


void InitMediaInfo()
{
#if 0
	auto videoInfo = std::make_unique<VideoInfo>();

	MediaInfo MI;
	if (MI.Open(L"D:\\Program\\Proxydomo with NicoCache\\Debug\\nico_cache\\sm26672100org_����TAS���惉���L���O 2015�N6����.mp4") == 0)
		return;

	videoInfo->width = std::stoi(MI.Get(Stream_Video, 0, _T("Width")).c_str());
	videoInfo->height = std::stoi(MI.Get(Stream_Video, 0, _T("Height")).c_str());

	videoInfo->formatProfile = MI.Get(Stream_Video, 0, _T("Format_Profile"));

	videoInfo->ref_frames = std::stoi(MI.Get(Stream_Video, 0, _T("Format_Settings_RefFrames")));

	std::wstring encodeSettings = MI.Get(Stream_Video, 0, _T("Encoded_Library_Settings"));
	// weightp=2 ����iPhone4S�ŉf�����~�܂��Ă��܂��H (��: sm26702656, sm26701432(High@L5.0�Ȃ̂ōĐ��ł��Ȃ�)
	auto pos = encodeSettings.find(L"weightb=");
	if (pos != std::wstring::npos) {
		std::wstring weightb = encodeSettings.substr(pos + 8, 1);
		videoInfo->weightb = std::stoi(weightb);
	} else {
		videoInfo->weightb = 0;
	}
#endif
}