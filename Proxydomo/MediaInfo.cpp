/**
*
*/

#include "stdafx.h"
#include "MediaInfo.h"
#include "..\libmediainfo_0.7.75_AllInclusive\MediaInfoLib\Source\MediaInfoDLL\MediaInfoDLL.h"
#pragma comment(lib, "MediaInfo.lib")
using namespace MediaInfoDLL;

// MI.Get の第三引数は 
// MediaInfoLib\Source\Resource\Text\Stream
// フォルダ以下にあるcsvファイルにどのテキストを書けばいいか分かる

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
	// weightp=2 だとiPhone4Sで映像が止まってしまう？ (例: sm26702656, sm26701432(High@L5.0なので再生できない)
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
	if (MI.Open(L"D:\\Program\\Proxydomo with NicoCache\\Debug\\nico_cache\\sm26672100org_月刊TAS動画ランキング 2015年6月号.mp4") == 0)
		return;

	videoInfo->width = std::stoi(MI.Get(Stream_Video, 0, _T("Width")).c_str());
	videoInfo->height = std::stoi(MI.Get(Stream_Video, 0, _T("Height")).c_str());

	videoInfo->formatProfile = MI.Get(Stream_Video, 0, _T("Format_Profile"));

	videoInfo->ref_frames = std::stoi(MI.Get(Stream_Video, 0, _T("Format_Settings_RefFrames")));

	std::wstring encodeSettings = MI.Get(Stream_Video, 0, _T("Encoded_Library_Settings"));
	// weightp=2 だとiPhone4Sで映像が止まってしまう？ (例: sm26702656, sm26701432(High@L5.0なので再生できない)
	auto pos = encodeSettings.find(L"weightb=");
	if (pos != std::wstring::npos) {
		std::wstring weightb = encodeSettings.substr(pos + 8, 1);
		videoInfo->weightb = std::stoi(weightb);
	} else {
		videoInfo->weightb = 0;
	}
#endif
}