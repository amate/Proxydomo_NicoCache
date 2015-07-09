/**
*	@file	NicoCacheManager.h
*/

#pragma once

#include <memory>
#include "proximodo\url.h"
#include "FilterOwner.h"
#include "Socket.h"


class CNicoCacheManager
{
public:
	static bool IsGetFlvURL(const CUrl& url);
	static void TrapGetFlv(CFilterOwner& filterOwner, std::unique_ptr<CSocket>& sockBrowser);
};














