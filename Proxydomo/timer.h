/**
*
*/

#pragma once

#include <string>
#include <boost\timer\timer.hpp>

class timer : public boost::timer::cpu_timer
{
public:
	timer(const std::string& format = "[%ws elapsed]") : m_format(format)
	{
		start();
	}

	std::string format() { return __super::format(6, m_format); }

private:
	std::string m_format;
};