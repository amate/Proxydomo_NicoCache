/**
*	@file	Matcher.cpp
*	@brief	
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

#include "Matcher.h"
#include "Nodes.h"
#include "proximodo\util.h"
#include "proximodo\const.h"	// for BIGNUMBER

namespace Proxydomo {

/* Searches a comma outside parentheses, within a command
 */
size_t findParamEnd(const std::string& str, char c) {
    int level = 0;
    size_t size = str.size();
    for (size_t pos = 0; pos < size; pos++) {
        switch (str[pos]) {
            case '\\': pos++;   break;
            case  '(': level++; break;
            case  ')': level--; break;
            case  ',': if (level<1) return pos; break;
        }
    }
    return string::npos;
}

/* Decodes a number in the pattern
 */
int readNumber(const std::string& pattern, int& pos, int stop, int &num) {

    int sign = 1;
    // There can be a minus sign
    if (pos < stop && pattern[pos] == '-') {
        sign = -1;
        ++pos;
    }
    num = 0;
    // At least one digit is needed
    if (pos == stop || !CUtil::digit(pattern[pos]))
        throw parsing_exception("NOT_A_NUMBER", pos);

    // Read all digits available
    for (; pos < stop && CUtil::digit(pattern[pos]); ++pos) {
        num = num*10 + pattern[pos] - '0';
    }
    // Return the corresponding number
    num *= sign;
    return num;
}


/* Decodes a range, in the form of spaces + (number or *) + spaces
 * possibly followed by the separation symbol + spaces + (number or *) + spaces
 */
void readMinMax(const std::string& pattern, int& pos, int stop, char sep,
                            int& min, int& max) {

    // consume spaces
    while (pos < stop && (pattern[pos] == ' ' || pattern[pos] == '\t')) ++pos;
    
    // read lower limit
    if (pos < stop && pattern[pos] == '*') {
        ++pos;
        min = -BIG_NUMBER;
        max = BIG_NUMBER;   // in case there is no upper limit
    } else if (pos < stop && pattern[pos] == sep) {
        min = -BIG_NUMBER;
        max = BIG_NUMBER;   // in case there is no upper limit
    } else {
        max = min = 0;
        readNumber(pattern, pos, stop, min);
        max = min;          // in case there is no upper limit
    }
    
    // consume spaces
    while (pos < stop && (pattern[pos] == ' ' || pattern[pos] == '\t')) ++pos;

    // check if there is an upper limit
    if (pos < stop && (pattern[pos] == sep || pattern[pos] == '-')) {
        ++pos;

        // consume spaces
        while (pos < stop && (pattern[pos] == ' ' || pattern[pos] == '\t')) ++pos;

        // read upper limit
        if (pos < stop && pattern[pos] == '*') {
            ++pos;
            max = BIG_NUMBER;
        } else if (pos < stop && pattern[pos] != '-' && !CUtil::digit(pattern[pos])) {
            max = BIG_NUMBER;
        } else {
            readNumber(pattern, pos, stop, max);
            if (max<min) { int i = max; max = min; min = i; }
        }

        // consume spaces
        while (pos < stop && (pattern[pos] == ' ' || pattern[pos] == '\t')) ++pos;
    }
}

/* Decodes a command name and content starting at position pos
 * (which should be pointing to the first letter of the command name).
 * Returns -1 if the function name is not known or () are not
 * corresponding. Else returns the position after the ).
 * command and content will contain the name and content.
 */
int decodeCommand(const std::string& pattern, int pos, int stop,
							std::string& command, std::string& content) {
    int index = pos;
    // Read command name
    while (index < stop && pattern[index] >= 'A' && pattern[index] <= 'Z') index++;
    // It should exist and be followed by (
    if (index == pos || index == stop || pattern[index] != '(') return -1;
    // Save the name
    command = pattern.substr(pos, index - pos);
    // Search for the closing )
    pos = ++index;
    int level = 1;  // Level of parentheses
    for (; index < stop && level > 0; index++) {
        // escaped () don't count
        if (pattern[index] == '(' && pattern[index-1] != '\\') {
            level++;
        } else if (pattern[index] == ')' && pattern[index-1] != '\\') {
            level--;
        }
    }
    // Did we reach end of string before closing all parentheses?
    if (level > 0) return -1;
    // Save content and return position
    content = pattern.substr(pos, index - pos - 1);
    return index;
}



///////////////////////////////////////////////////////////////////
// CMatcher

/* Constructor
 * It stores the requested url and transforms the pattern into
 * a search tree. An exception is returned if the pattern is
 * malformed.
 */
/// pattern を検索木に変換します。 patternが無効なら例外が飛びます。
CMatcher::CMatcher(const string& pattern)
{
    // position up to which the pattern is decoded
    int pos = 0;

    if (pattern.size() > 0 && pattern[0] == '^') {
        // special case: inverted pattern
		m_root.reset(new CNode_Negate(expr(pattern, ++pos, pattern.size())));
    } else {
        // (this call may throw an exception)
        m_root.reset(expr(pattern, pos, pattern.size())); 
    }
    m_root->setNextNode();

    // another reason to throw : the pattern was not completely
    // consumed, i.e there was an unpaired )
	/// パターンが完全に消費されていない。※例: ()がペアになっていない
    if ((size_t)pos != pattern.size()) {
        throw parsing_exception("PARSING_INCOMPLETE", pos);
    }
}


/* Destructor
 */
CMatcher::~CMatcher()
{
}


std::shared_ptr<CMatcher>	CMatcher::CreateMatcher(const std::string& pattern)
{
	try {
		return std::shared_ptr<CMatcher>(new CMatcher(pattern));
	} catch (parsing_exception& e) {
		//ATLTRACE("CreateMatcher parsererro pattern(%s) : err %s [%d]", pattern.c_str(), e.message.c_str(), e.position);
	}
	return nullptr;
}

std::shared_ptr<CMatcher>	CMatcher::CreateMatcher(const std::string& pattern, std::string& errmsg)
{
	try {
		return std::shared_ptr<CMatcher>(new CMatcher(pattern));
	} catch (parsing_exception& e) {
		errmsg = e.message;
		char strpos[24];
		::_itoa_s(e.position, strpos, 10);
		errmsg += "\nposition: ";
		errmsg += strpos;
		errmsg += "\n";
	}
	return nullptr;
}


/* Invokes the root node of the tree, to try and match the text.
 * start: position in html from which to scan
 * stop:  position in html where to stop scanning
 * end:   end position of the match
 * reached: farthest character scanned (e.g =stop for pattern "*")
 * Returns the end position (end) of the match, if text[start,end] matches
 * the pattern. Otherwise returns -1
 * Memory stack and map will also contain strings matched by \0-9 or \#
 *
 * Note: The filter using match() MUST unlock the mutex if it is still
 * locked. It cannot be done from inside CMatcher (nor CExpander) because
 * of possible recursive calls.
 */
bool CMatcher::match(const char* start, const char* stop,
                     const char*& end, MatchData* pMatch)
{
    if (m_root == nullptr)
		return false;
    //this->reached = start;
	pMatch->reached = start;
    end = m_root->match(start, stop, pMatch);
    return end != nullptr;
}



/* This static version of the matching function builds a search tree
 * at each call. It is mainly for use in replacement functions, that
 * are not supposed to be called too often (only after a match has
 * been found with an instantiated CMatcher).
 */
bool CMatcher::match(const string& pattern, CFilter& filter,
                     const char* start, const char* stop,
                     const char*& end, const char*& reached) {
    try {
        CMatcher matcher(pattern);
		MatchData matchData(&filter);
        return matcher.match(start, stop, end, &matchData);
    } catch (parsing_exception e) {
        return false;
    }
}

/* mayMatch() creates and returns a table of expected characters
 * at the beginning of the text to match. It can be called after
 * the construction of the CMatcher, to provide the search engine
 * with a fast way to know if a call to CMatcher::match() is of any
 * use. If !tab[text[start]] the engine knows there cannot be any
 * match at start, _even_ if the buffer is smaller than the window
 * size.
 */
void CMatcher::mayMatch(bool* tab)
{
    for (int i=0; i<256; i++) tab[i] = false;
    if (m_root->mayMatch(tab))
        // The whole pattern can match without consuming anything,
        // we'll have to try it systematically.
        for (int i=0; i<256; i++) tab[i] = true;
}


/* Tells if pattern is * or \0-9
 */
bool CMatcher::isStar() 
{
    return (m_root && (m_root->m_id == CNode::STAR || m_root->m_id == CNode::MEMSTAR));
}


/* Builds a tree node for the expression beginning at position pos.
 * We split and interpret the pattern where there is && (level 0)
 * & (level 1) or | (level 2). This allows operator precedence:
 * "|" > "&" > "&&"
 */
CNode* CMatcher::expr(const std::string& pattern, int& pos, int stop, int level)
{

    if (level == 2) {

        CNode *node = run(pattern, pos, stop);

        if (pos < stop && pattern[pos] == '|') {
            // Rule: |
            // We need an alternative of several runs. We will place them
            // all in a vector, then build a CNode_Or with the vector content.
            std::vector<CNode*> *vect = new std::vector<CNode*>();
            vect->push_back(node);
            
            // We continue getting runs as long as there are |
            while (pos < stop &&  pattern[pos] == '|') {
                ++pos;
                try {
                    node = run(pattern, pos, stop);
                } catch (parsing_exception e) {
                    // Clean before throwing exception
                    CUtil::deleteVector<CNode>(*vect);
                    delete vect;
                    throw e;
                }
                vect->push_back(node);
            }
            
            // Build the CNode_Or will all the alternatives.
            node = new CNode_Or(vect);
        }
        return node;
    }

    CNode *node = expr(pattern, pos, stop, level + 1);
    
    while (pos < stop && pattern[pos] == '&' &&
           ((level == 0 && pos+1 < stop && pattern[pos+1] == '&') ||
            (level == 1 && (pos+1 == stop || pattern[pos+1] != '&')) ) ) {

        // Rules: & &&
        // & is a binary operator, so we read the following run and
        // make a CNode_And with left and right runs.
        pos += (level == 0 ? 2 : 1);
        try {
            CNode *node2 = expr(pattern, pos, stop, level + 1); // &'s right run
            // Build the CNode_And with left and right runs.
            node = new CNode_And(node, node2, (level == 0));
        } catch (parsing_exception e) {
            delete node;
            throw e;
        }
    }
    // Node contains the tree for all that we decoded from old pos to new pos
    return node;
}


/* Builds a node for the run beginning at position pos
 * A 'run' is a list of codes (escaped codes, characters,
 * expressions in (), ...) that should be matched one after another.
 */
CNode* CMatcher::run(const string& pattern, int& pos, int stop)
{
    CNode* node = code(pattern, pos, stop);
    std::vector<CNode*>* v = new std::vector<CNode*>();     // the list of codes

    while (node) {
        // Rule: +
        // If the code is followed by +, we will embed the code in a
        // repetition node (Node_Repeat). But first we check what follows
        // to see if parameters should be provided to the CNode_Repeat.
        if (pos < stop && pattern[pos] == '+') {
            ++pos;
            // Rule: ++
            // is it a ++?
            bool iter = false;
            int rmin = 0, rmax = BIG_NUMBER;
            if (pos < stop && pattern[pos] == '+') {
                ++pos;
                iter = true;
            }
            // Rule: {}
            // Is a range provided?
            if (pos < stop && pattern[pos] == '{') {
                ++pos;
                try {
                    // Read the values
                    readMinMax(pattern, pos, stop, ',', rmin, rmax);
                    if (rmin < 0) rmin = 0;
                    if (rmax < rmin) rmax = rmin;

                    // check that the curly braces are closed
                    if (pos < stop && pattern[pos] == '}') {
                        ++pos;
                    } else {
                        throw parsing_exception("MISSING_CURLY", pos);
                    }
                } catch (parsing_exception e) {
                    CUtil::deleteVector<CNode>(*v);
                    delete v;
                    delete node;
                    throw e;
                }
            }
            // Embed node in a repetition node
            node = new CNode_Repeat(node, rmin, rmax, iter);
        }

        // Let's do now an obvious optimization: consecutive
        // chars will not be matched one by one (too many function
        // calls) but as single string.
        if (node->m_id == CNode::CHAR && !v->empty()) {
            CNode_Char *nodeChar = (CNode_Char*)node;
            char newChar = nodeChar->getChar();

            CNode *prevNode = v->back();
            if (prevNode->m_id == CNode::CHAR) {
                CNode_Char *prevNodeChar = (CNode_Char*)prevNode;

                // consecutive CNode_Char's become one CNode_String
                string str;
                str  = prevNodeChar->getChar();
                str += newChar;
                delete prevNode;
                v->pop_back();
                delete node;
                node = new CNode_String(str);

            } else if (prevNode->m_id == CNode::STRING) {
                CNode_String *prevNodeString = (CNode_String *)prevNode;

                // append CNode_Char to previous CNode_String
                prevNodeString->append(newChar);
                v->pop_back();
                delete node;
                node = prevNodeString;
            }
        }

        v->push_back(node);
        try {
            node = code(pattern, pos, stop);
        } catch (parsing_exception e) {
            CUtil::deleteVector<CNode>(*v);
            delete v;
            throw e;
        }
    }
    // (end of the run)
    
    if (v->size() == 0) {

        // Empty run, just record that as an empty node
        delete v;
        return new CNode_Empty();
        
    } else if (v->size() == 1) {

        // Only 1 code, we don't need to embed it in a CNode_Run
        node = (*v)[0];
        delete v;
        return node;
        
    } else {

        // Now scan the whole list of codes to associate quotes by pairs.
        // We don't care about the quotes being ' or ". We just have to
        // associate first with second, third with fourth, and so on.
        // The association is materialized by second (resp. fourth) being
        // given a reference to first (resp. third).
        CNode_Quote* quote = NULL; // this points to the previous unpaired quote
        for (auto it = v->begin(); it != v->end(); ++it) {
            if ((*it)->m_id == CNode::QUOTE) {
                if (quote) {
                    ((CNode_Quote*)(*it))->setOpeningQuote(quote);
                    quote = NULL;
                } else {
                    quote = (CNode_Quote*)(*it);
                }
            }
        }
        
        // Returns a CNode_Run embedding the vector of Nodes
        return new CNode_Run(v);
    }
}


/* Builds a node for a single code beginning at position pos. A code can be:
 * - an escaped code \x
 * - an alternative of characters [ ] or [^ ]
 * - a numerical range [# ] or [^# ]
 * - a command $NAME( )
 * - an expression ( )
 * - an expression to memorize ( )\n
 * - a single character x
 */
CNode* CMatcher::code(const string& pattern, int& pos, int stop)
{
    // CR and LF are only for user convenience, they have no meaning whatsoever
	// CR や LF はユーザーにとって便利なだけで、構文的には何の意味も持たないので飛ばす
    while (pos < stop && (pattern[pos] == '\r' || pattern[pos] == '\n')) pos++;

    // This NULL will be interpreted by run() as end of expression
    if (pos == stop) 
		return nullptr;

    // We look at the first caracter
    char token = pattern[pos];
    
    // Escape code
    if (token == '\\') {

        // it should not be at the end of the pattern
		// '\'でパターンが終わっていればおかしいので例外を飛ばす
        if (pos+1 == stop) 
			throw parsing_exception("ESCAPE_AT_END", pos);

        token = pattern[pos+1];
        pos += 2;

        switch (token) {
            case 't':
                // Rule: \t
                return new CNode_Char('\t');
            case 'r':
                // Rule: \r
                return new CNode_Char('\r');
            case 'n':
                // Rule: \n
                return new CNode_Char('\n');
            case 's':
                // Rule: \s
                return new CNode_Repeat(new CNode_Chars(" \t\r\n"), 1, BIG_NUMBER);
            case 'w':
                // Rule: \w
                return new CNode_Repeat(new CNode_Chars(" \t\r\n>", false), 0, BIG_NUMBER);
            case '#':
                // Rule: \#
                return new CNode_MemStar();
            case 'k':
                // Rule: \k
                return new CNode_Command(CMD_KILL, "", ""/*, filter*/);
            default:
                // Rule: \0-9
                if (CUtil::digit(token))
                    return new CNode_MemStar(token - '0');

                // Rules: \u \h \p \a \q
                if (string("uhpaq").find(token, 0) != string::npos)
                    return new CNode_Url(token);

                // Rule: backslash
                return new CNode_Char(token);
        }
        
    // Range or list of characters
    } else if (token == '[') {

        ++pos;
        // Rule: [^
        // If the [] is negated by ^, keep that information
        bool allow = true;
        if (pos < stop && pattern[pos] == '^') {
            allow = false;
            ++pos;
        }
        
        // Range
        if (pos < stop && pattern[pos] == '#') {

            ++pos;
            // read the range. pos is updated to the closing ]
            int rmin, rmax;
            readMinMax(pattern, pos, stop, ':', rmin, rmax);
            if (pos == stop || pattern[pos] != ']')
                throw parsing_exception("MISSING_CROCHET", pos);
            ++pos;
            // Rule: \n
            return new CNode_Range(rmin, rmax, allow);

        // Alternative of characters
        } else {

            // Rule: []
            // We create a clear CNode_Chars then provide it with
            // characters one by one.
            CNode_Chars *node = new CNode_Chars("", allow);
            
            unsigned char prev = 0, code;
            bool prevCase = false, codeCase, inRange = false;
            for (; pos < stop && (code = pattern[pos]) != ']'; ++pos) {

                if (code == '%' && pos+2 < stop
                        && CUtil::hexa(pattern[pos+1])
                        && CUtil::hexa(pattern[pos+2])) {

                    // %xx notation (case-sensitive)
                    char c = toupper(pattern[++pos]);
                    code = (c <= '9' ? c - '0' : c - 'A' + 10) * 16;
                    c = toupper(pattern[++pos]);
                    code += (c <= '9' ? c - '0' : c - 'A' + 10);
                    codeCase = true;
                    
                } else if (code == '\\' && pos+1 < stop) {

                    // escaped characters must be decoded (case-sensitive)
					++pos;
                    code = pattern[pos];
                    switch (code) {
                        case 't' : code = '\t'; break;
                        case 'r' : code = '\r'; break;
                        case 'n' : code = '\n'; break;
                        // Only those three are not to be taken as is
                    }
                    codeCase = true;

                } else {

                    // normal character
                    codeCase = false;
                }
                
                if (inRange) {

                    // We decoded both ends of a range
                    if (prevCase || codeCase) {
                        // Add characters as is
                        for (; prev<=code; prev++) {
                            node->add(prev);
                        }
                    } else {
                        // Add both uppercase and lowercase characters
                        prev = tolower(prev);
                        unsigned char cmax = tolower(code);
                        for (; prev<=cmax; prev++) {
                            node->add(prev);
                            node->add(toupper(prev));
                        }
                    }
                    inRange = false;
                    
                } else if (pos+2 < stop
                        && pattern[pos+1] == '-'
                        && pattern[pos+2] != ']') {

                    // We just decoded the start of a range, record it but don't add it
                    inRange = true;
                    prev = code;
                    prevCase = codeCase;
                    pos++;
                    
                } else {

                    // The decoded character is not part of a range
                    if (codeCase) {
                        node->add(code);
                    } else {
                        node->add(tolower(code));
                        node->add(toupper(code));
                    }
                }
            }
            // Check if the [] is closed
			// ']'が来る前にパターンが終わったので例外を飛ばす
            if (pos == stop) {
                delete node;
                throw parsing_exception("MISSING_CROCHET", pos);
            }
            ++pos;
            return node;
        }
        
    // Command
    } else if (token == '$') {

        // Rule: $COMMAND(,,)
        // Decode command name and content
        // Note: unknown commands are ignored (just as a $DONOTHING(someignoredtext))
        string command, content;
        int endContent = decodeCommand(pattern, pos+1, stop, command, content); // after )
        int contentSize = content.size();
        int startContent = endContent - 1 - contentSize; // for exception info
        
        if (endContent<0) {
            // Do as if it was not a command, the token is a normal char
            ++pos;
            return new CNode_Char('$');
        }

        // The pattern will continue after the closing )
        pos = endContent;

        try {
            if (command == "AV") {

                // Command "match a tag parameter"
                // Transform content of () in a tree and embed it in
                // a fast quote searcher
                int start = 0;
                return new CNode_AV(expr(content, start, contentSize), false);

            } else if (command == "AVQ") {

                // Command "match a tag parameter, including optional quotes"
                // Transform content of () in a tree and embed it in a fast
                // quote searcher
                int start = 0;
                return new CNode_AV(expr(content, start, contentSize), true);

            } else if (command == "LST") {

                // Command to try a list of patterns.
                // The patterns are found in CSettings, and nodes are created
                // at need, and kept until the matcher is destroyed.
                return new CNode_List(content/*, *this*/);

            } else if (command == "SET") {

                // Command to set a variable
                size_t eq = content.find('=');
                if (eq == string::npos)
                    throw parsing_exception("MISSING_EQUAL", 0);
                std::string name = content.substr(0, eq);
                std::string value = content.substr(eq+1);
                CUtil::trim(name);
                CUtil::lower(name);
                if (name[0] == '\\') name.erase(0,1);
                if (name == "#") {
                    return new CNode_Command(CMD_SETSHARP, "", value/*, filter*/);
                } else if (name.length() == 1 && CUtil::digit(name[0])) {
                    return new CNode_Command(CMD_SETDIGIT, name, value/*, filter*/);
                } else {
                    return new CNode_Command(CMD_SETVAR, name, value/*, filter*/);
                }

            } else if (command == "TST") {

                // Command to try and match a variable
                int eq, level;
                for (eq = 0, level = 0; eq < contentSize; eq++) {
                    // Left parameter can be some text containing ()
                    if (content[eq] == '(' && (!eq || content[eq-1]!='\\'))
                        level++;
                    else if (content[eq] == ')' && (!eq || content[eq-1]!='\\'))
                        level--;
                    else if (content[eq] == '=' && !level)
                        break;
                }
                
                if (eq == contentSize) {
                    CUtil::trim(content);
                    if (content[0] != '(') CUtil::lower(content);
                    if (content[0] == '\\') content.erase(0,1);
                    return new CNode_Test(content/*, filter*/);
                }
                startContent += eq + 1;
                string name = content.substr(0, eq);
                string value = content.substr(eq+1);
                CUtil::trim(name);
                if (name[0] != '(') CUtil::lower(name);
                if (name[0] == '\\') name.erase(0,1);
                if (name == "#") {
                    return new CNode_Command(CMD_TSTSHARP, "", value/*, filter*/);
                } else if (name.length() == 1 && CUtil::digit(name[0])) {
                    return new CNode_Command(CMD_TSTDIGIT, name, value/*, filter*/);
                } else if (name[0] == '(' && name[name.size()-1] == ')') {
                    name = name.substr(1, name.size()-2);
                    return new CNode_Command(CMD_TSTEXPAND, name, value/*, filter*/);
                } else {
                    return new CNode_Command(CMD_TSTVAR, name, value/*, filter*/);
                }

            } else if (command == "ADDLST") {

                // Command to add a line to a list
                size_t comma = content.find(',');
                if (comma == string::npos)
                    throw parsing_exception("MISSING_COMMA", 0);
                string name = content.substr(0, comma);
                string value = content.substr(comma + 1);
                CUtil::trim(name);
                return new CNode_Command(CMD_ADDLST, name, value/*, filter*/);

            } else if (command == "ADDLSTBOX") {

                // Command to add a line to a list after user confirm & edit
                size_t comma = content.find(',');
                if (comma == string::npos)
                    throw parsing_exception("MISSING_COMMA", 0);
                string value = content.substr(comma + 1);
                string name = content.substr(0, comma);
                CUtil::trim(name);
                return new CNode_Command(CMD_ADDLSTBOX, name, value/*, filter*/);

            } else if (command == "URL") {

                // Command to try and match the document URL
                return new CNode_Command(CMD_URL, "", content/*, filter*/);

            } else if (command == "TYPE") {

                // Command to check the downloaded file type
                CUtil::trim(content);
                CUtil::lower(content);
                return new CNode_Command(CMD_TYPE, "", content/*, filter*/);

            } else if (command == "IHDR") {

                // Command to try and match an incoming header
                size_t colon = content.find(':');
                if (colon == string::npos)
                    throw parsing_exception("MISSING_COLON", 0);
                string name = content.substr(0, colon);
                string value = content.substr(colon+1);
                CUtil::trim(name);
                CUtil::lower(name);
                return new CNode_Command(CMD_IHDR, name, value/*, filter*/);

            } else if (command == "OHDR") {

                // Command to try and match an outgoing header
                size_t colon = content.find(':');
                if (colon == string::npos)
                    throw parsing_exception("MISSING_COLON", 0);
                string name = content.substr(0, colon);
                string value = content.substr(colon+1);
                CUtil::trim(name);
                CUtil::lower(name);
                return new CNode_Command(CMD_OHDR, name, value/*, filter*/);

            } else if (command == "RESP") {

                // Command to try and match the response line
                return new CNode_Command(CMD_RESP, "", content/*, filter*/);

            } else if (command == "ALERT") {

                // Command to display a message box
                return new CNode_Command(CMD_ALERT, "", content/*, filter*/);

            } else if (command == "CONFIRM") {

                // Command to ask the user for a choice (Y/N)
                return new CNode_Command(CMD_CONFIRM, "", content/*, filter*/);

            } else if (command == "STOP") {

                // Command to stop the filter from filtering
                return new CNode_Command(CMD_STOP, "", ""/*, filter*/);

            } else if (command == "USEPROXY") {

                // Command to set if the proxy settings will be used
                CUtil::trim(content);
                CUtil::lower(content);
                if (content != "true" && content != "false")
                    throw parsing_exception("NEED_TRUE_FALSE", 0);
                return new CNode_Command(CMD_USEPROXY, "", content/*, filter*/);

            } else if (command == "SETPROXY") {

                // Command to force the use of a proxy. The command
                // will have no effect if, when matching, there is no corresponding
                // proxy is the settings.
                CUtil::trim(content);
                if (content.empty())
                    throw parsing_exception("NEED_TEXT", 0);
                return new CNode_Command(CMD_SETPROXY, "", content/*, filter*/);
                
            } else if (command == "CON") {
#if 0
                // Command to count requests. Matches if (reqNum-1)/z % y = x-1
                int x, y, z;
                try {
                    x = y = z = 0;
                    size_t comma = content.find(',');
                    if (comma == string::npos) throw (int)1;
                    stringstream ss;
                    ss << content.substr(0,comma);
                    ss >> x;
                    if (x<1) throw (int)2;
                    content = content.substr(comma+1);
                    comma = content.find(',');
                    if (comma != string::npos) {
                        ss.clear();
                        ss.str(content.substr(comma+1));
                        ss >> z;
                        if (z<1) throw (int)3;
                        content = content.substr(0, comma);
                    } else {
                        z = 1;
                    }
                    ss.clear();
                    ss.str(content);
                    ss >> y;
                    if (y<1) throw (int)4;
                } catch (int err) {
                    throw parsing_exception("TWO_THREE_INTEGERS", 0);
                }
                return new CNode_Cnx(reached, x, y, z,
                                     filter.owner.cnxNumber);
#endif
				
            } else if (command == "LOG") {

                // Command to send an log event to the log system
                return new CNode_Command(CMD_LOG, "", content/*, filter*/);

            } else if (command == "JUMP") {

                // Command to tell the browser to go to another URL
                CUtil::trim(content);
                return new CNode_Command(CMD_JUMP, "", content/*, filter*/);

            } else if (command == "RDIR") {

                // Command to transparently redirect to another URL
                CUtil::trim(content);
                return new CNode_Command(CMD_RDIR, "", content/*, filter*/);

            } else if (command == "FILTER") {

                // Command to force body filtering regardless of its type
                CUtil::trim(content);
                CUtil::lower(content);
                if (content != "true" && content != "false")
                    throw parsing_exception("NEED_TRUE_FALSE", 0);
                return new CNode_Command(CMD_FILTER, "", content/*, filter*/);

            } else if (command == "LOCK") {

                // Command to lock the filter mutex
                return new CNode_Command(CMD_LOCK,"", ""/*, filter*/);

            } else if (command == "UNLOCK") {

                // Command to lock the filter mutex
                return new CNode_Command(CMD_UNLOCK,"", ""/*, filter*/);

            } else if (command == "KEYCHK") {

                // Command to lock the filter mutex
                CUtil::upper(content);
                return new CNode_Command(CMD_KEYCHK,"", content/*, filter*/);

            } else if (command == "NEST" ||
                       command == "INEST") {

                // Command to match nested tags (with optional content)
                size_t colon = findParamEnd(content, ',');
                if (colon == string::npos)
                    throw parsing_exception("MISSING_COMMA", 0);
                bool hasMiddle = false;
                std::string text1 = content.substr(0, colon);
                std::string text2;
                std::string text3 = content.substr(colon + 1);
                colon = findParamEnd(text3, ',');
                if (colon != string::npos) {
                    text2 = text3.substr(0, colon);
                    text3 = text3.substr(colon + 1);
                    hasMiddle = true;
                }
                CNode *left = NULL, *middle = NULL, *right = NULL;
                try {
                    int end;
                    left = expr(text1, end=0, text1.size());
                    if (hasMiddle) middle = expr(text2, end=0, text2.size());
                    right = expr(text3, end=0, text3.size());
                } catch (parsing_exception e) {
                    if (left)   delete left;
                    if (middle) delete middle;
                    if (right)  delete right;
                    throw e;
                }
                return new CNode_Nest(left, middle, right, command == "INEST");

            } else if (command == "ASK") {

                // Command to automate the insertion of an item in one of 2 lists
                size_t colon = content.find(',');
                if (colon == string::npos)
                    throw parsing_exception("MISSING_COMMA", 0);
                string allowName = content.substr(0, colon);
                content.erase(0, colon + 1);
                colon = content.find(',');
                if (colon == string::npos)
                    throw parsing_exception("MISSING_COMMA", 0);
                string denyName = content.substr(0, colon);
                content.erase(0, colon + 1);
                colon = findParamEnd(content, ',');
                if (colon == string::npos)
                    throw parsing_exception("MISSING_COMMA", 0);
                string question = content.substr(0, colon);
                string item = content.substr(colon + 1);
                string pattern = "\\h\\p\\q\\a";
                colon = findParamEnd(item, ',');
                if (colon != string::npos) {
                    pattern = item.substr(colon + 1);
                    item = item.substr(0, colon);
                }
                CUtil::trim(allowName);
                CUtil::trim(denyName);
                CUtil::trim(question);
                CUtil::trim(item);
                CUtil::trim(pattern);
                return new CNode_Ask(/*filter, */allowName, denyName, question, item, pattern);

            } else {

                // Unknown command
                throw parsing_exception("UNKNOWN_COMMAND", -1);
            }
        } catch (parsing_exception e) {
            // Error: adjust errorPos and forward error
            e.position += startContent;
            throw e;
        }
    // Expression
    } else if (token == '(') {

        // Rule: ()
        ++pos;
        // Detect a negation symbol
        bool negate = false;
        if (pos < stop && pattern[pos] == '^') {
            ++pos;
            negate = true;
        }
            
        // We read the enclosed expression
        CNode *node = expr(pattern, pos, stop);
        
        // It must be followed by the closing )
		// ')'がこない、もしくは 次のトークンに')'が来ないと例外を飛ばす
        if (pos == stop || pattern[pos] != ')') {
            delete node;
            throw parsing_exception("MISSING_PARENTHESE", pos);
        }
        ++pos;

        // Rule: (^ )
        // If the expression must be negated, embed it in a CNode_Negate
        if (negate)
            node = new CNode_Negate(node);

        if (pos+1 < stop && pattern[pos]=='\\') {
            if (CUtil::digit(pattern[pos+1])) {
                // Rule: ()\0-9
                // The code will be embedded in a memory node
                int num = pattern[pos+1]-'0';
                node = new CNode_Memory(node, num);

                pos += 2;
            } else if (pattern[pos+1] == '#') {
                // Rule: ()\#
                // The code will be embedded in a stacked-memory node
                node = new CNode_Memory(node, -1);
                pos += 2;
            }
        }

        return node;

    // End of expression
    } else if (token == ')' || token == '|' || token == '&') {

        return NULL;    // This NULL will be interpreted by run()
        
    }

    // Single character (which can have a special meaning)
    return single(pattern, pos, stop);
}


/* Builds a node for a single symbol at position pos. It can be
 * a character with a special meaning (space, =, ?, *, ', ") or
 * a ascii character to be matched (case insensitive).
 */
CNode* CMatcher::single(const std::string& pattern, int& pos, int stop) {

    // No need to test for pos<stop here, the test is done in code()
    char token = pattern[pos];
	++pos;

    if (token == '?') {

        // Rule: ?
        return new CNode_Any();
        
    } else if (token == ' ' || token == '\t') {

        // Only one CNode_Space for consecutive spaces
        while (pos < stop && pattern[pos] <= ' ') ++pos;
        
        // No need creating a CNode_Space for spaces before = in the pattern
        if (pos < stop && pattern[pos] == '=')
            return single(pattern, pos, stop);
            
        // Rule: space
        return new CNode_Space();

    } else if (token == '=') {

        // Rule: =
        while (pos < stop && pattern[pos] <= ' ') ++pos;
        return new CNode_Equal();

    } else if (token == '*') {

        // Rule: *
        return new CNode_Star();

    } else if (token == '\'' || token == '\"') {

        // Rule: "
        // Rule: '
        return new CNode_Quote(token);

    } else {

        // ascii character
        return new CNode_Char(tolower(token));
    }
}



}	// namespace Proxydomo
















