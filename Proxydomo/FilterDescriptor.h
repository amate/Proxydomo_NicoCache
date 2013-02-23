/**
*	@file	FilterDescriptor.h
*	@brief	�t�B���^�[ ���\��
*/
/**
	this file is part of Proxydomo
	Copyright (C) amate 2013-

	This program is free software; you can redistribute it and/or
	modify it under the terms of the GNU General Public License
	as published by the Free Software Foundation; either
	version 2 of the License, or (at your option) any later version.

	This program is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with this program; if not, write to the Free Software
	Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
*/
#pragma once

#include <string>

class CFilterDescriptor
{
public:

	CFilterDescriptor();

	/// Clear all content
	void	Clear();

	// �����Ȃ�errorMsg�ɃG���[������
    void	TestValidity();

	/// �t�B���^�[���L���ł���
	bool	Active;

    // The following data is used for organizing/editing filters
    //int id;             // Unique ID of the filter
    std::string title;       // Title of the filter
    std::string version;     // Version number
    std::string author;      // Name of author
    std::string comment;     // Comment (such as description of what the filter does)
    //int folder;         // parent folder's id

    // Type of filter
    enum FilterType { kFilterText, kFilterHeadOut, kFilterHeadIn };
    FilterType	filterType;

    // Data specific to header filters
    std::string headerName;

    // Data specific to text filters
    bool   multipleMatches;
    int    windowWidth;
    std::string boundsPattern;

    // Data commom to both
    std::string urlPattern;
    std::string matchPattern;
    std::string replacePattern;
    //int    priority;
    
    // Default filter number (set to 0 for new/modified filters)
    //int defaultFilter;
    
    // Check if all data is valid
    std::string errorMsg;

};