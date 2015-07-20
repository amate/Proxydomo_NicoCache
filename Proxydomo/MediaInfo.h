/**
*
*/

#pragma once

#include <string>
#include <memory>

struct VideoInfo
{
	int	width;
	int height;
	float fps;

	std::wstring formatProfile;
	int ref_frames;
	int weightb;
};

std::unique_ptr<VideoInfo>	GetVideoInfo(const std::wstring& filePath);

void InitMediaInfo();



















