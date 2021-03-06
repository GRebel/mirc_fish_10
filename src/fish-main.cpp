#include "fish-internal.h"


static CRITICAL_SECTION s_iniLock;

static std::shared_ptr<CBlowIni> GetBlowIni()
{
	static std::shared_ptr<CBlowIni> ls_instance;

	if(!ls_instance)
	{
		::EnterCriticalSection(&s_iniLock);
		// the lock makes sure we only ever get one CBlowIni instance.
		if(!ls_instance) ls_instance = std::shared_ptr<CBlowIni>(new CBlowIni());
		::LeaveCriticalSection(&s_iniLock);
	}

	return ls_instance;
}


std::map<HANDLE, std::string> s_socketMap;
static CRITICAL_SECTION s_socketMapLock;


/* for use from fish_inject.dll */
EXPORT_SIG(__declspec(dllexport) char*) _OnIncomingIRCLine(HANDLE a_socket, const char* a_line, size_t a_len)
{
	if(*a_line != ':')
		return NULL;

	if(strstr(a_line, " ") == strstr(a_line, " 005 "))
	{
		std::string l_line(a_line, a_len);
		std::string::size_type l_pos = l_line.find(" NETWORK=");

		if(l_pos != std::string::npos)
		{
			l_pos += 9; // strlen(" NETWORK=")
			std::string::size_type l_endPos = l_line.find(" ", l_pos);
			if(l_endPos == std::string::npos) l_endPos = l_line.size();

			::EnterCriticalSection(&s_socketMapLock);
			// allow overwriting network names for dis/re-connecting BNCs and such:
			s_socketMap[a_socket] = l_line.substr(l_pos, l_endPos - l_pos);
			::LeaveCriticalSection(&s_socketMapLock);

			return NULL;
		}
	}

	// quick exit to save some CPU cycles:
	if(!strstr(a_line, "+OK ") && !strstr(a_line, "mcps "))
		return NULL;

	auto l_ini = GetBlowIni();

	if(!l_ini->GetBool(L"process_incoming", true))
		return NULL;

	/** list of stuff we possibly need to decrypt: **
		:nick!ident@host PRIVMSG #chan :+OK 2T5zD0mPgMn
		:nick!ident@host PRIVMSG #chan :\x01ACTION +OK 2T5zD0mPgMn\x01
		:nick!ident@host PRIVMSG ownNick :+OK 2T5zD0mPgMn
		:nick!ident@host PRIVMSG ownNick :\x01ACTION +OK 2T5zD0mPgMn\x01
		:nick!ident@host NOTICE ownNick :+OK 2T5zD0mPgMn
		:nick!ident@host NOTICE #chan :+OK 2T5zD0mPgMn
		:nick!ident@host NOTICE @#chan :+OK 2T5zD0mPgMn
		:nick!ident@host NOTICE ~#chan :+OK 2T5zD0mPgMn
		(topic) :irc.tld 332 nick #chan :+OK hqnSD1kaIaE00uei/.3LjAO1Den3t/iMNsc1
		:nick!ident@host TOPIC #chan :+OK JRFEAKWS
		(topic /list) :irc.tld 322 nick #chan 2 :[+snt] +OK BLAH
	*/

	std::string l_line(a_line, a_len);
	std::string l_cmd, l_contact, l_message;
	std::string::size_type l_cmdPos, l_tmpPos, l_targetPos, l_msgPos;

	StrTrimRight(l_line);

	l_cmdPos = l_line.find(' ');
	if(l_cmdPos != std::string::npos)
	{
		while(l_line[l_cmdPos] == ' ') l_cmdPos++;
		l_tmpPos = l_line.find(' ', l_cmdPos);

		if(l_tmpPos != std::string::npos)
		{
			while(l_line[l_tmpPos + 1] == ' ') l_tmpPos++;
			l_cmd = l_line.substr(l_cmdPos, l_tmpPos - l_cmdPos);

			l_msgPos = l_line.find(" :", l_tmpPos + 1);

			if(l_msgPos != std::string::npos)
			{
				l_msgPos += 2;
				l_message = l_line.substr(l_msgPos);
			}
		}
	}

	if(l_cmd.empty() || l_message.empty())
		return NULL;

	// check if +OK is in the message part of the line:
	if(l_message.find("+OK ") == std::string::npos && l_message.find("mcps ") == std::string::npos)
		return NULL;

	enum {
		CMD_PRIVMSG = 1,
		CMD_ACTION,
		CMD_NOTICE,
		CMD_N332, // 332 channel :topic
		CMD_TOPIC,
		CMD_N322 // 322 channel users :topic
	} l_cmd_type;

	if(!_stricmp(l_cmd.c_str(), "PRIVMSG"))
		l_cmd_type = CMD_PRIVMSG;
	else if(!_stricmp(l_cmd.c_str(), "NOTICE"))
		l_cmd_type = CMD_NOTICE;
	else if(!strcmp(l_cmd.c_str(), "332"))
		l_cmd_type = CMD_N332;
	else if(!_stricmp(l_cmd.c_str(), "TOPIC"))
		l_cmd_type = CMD_TOPIC;
	else if(!strcmp(l_cmd.c_str(), "322"))
		l_cmd_type = CMD_N322;
	else
		return NULL;

	std::string l_leading, l_trailing;

	if(l_cmd_type == CMD_N322 || l_cmd_type == CMD_N332 || l_cmd_type == CMD_TOPIC)
	{
		l_targetPos = l_line.rfind(" #", l_msgPos);

		if(l_targetPos != std::string::npos && l_targetPos >= l_cmdPos + l_cmd.size())
			/* >= because of the leading space in the find string */
		{
			l_targetPos++; // skip the leading space
			l_tmpPos = l_line.find(' ', l_targetPos + 1);

			if(l_tmpPos != std::string::npos)
			{
				l_contact = l_line.substr(l_targetPos, l_tmpPos - l_targetPos);
			}
		}

		if(l_cmd_type == CMD_N322 && !l_message.empty())
		{
			// account for channel modes in /list, like "[+nts] +OK BLAH"
			if(l_message[0] == '[')
			{
				l_tmpPos = l_message.find("] +OK ");

				if(l_tmpPos != std::string::npos)
				{
					l_leading = l_message.substr(0, l_tmpPos + 2);
					l_message.erase(0, l_tmpPos + 2);
				}
			}
		}
	}
	else
	{
		l_contact = l_line.substr(l_tmpPos + 1, l_msgPos - 2 - l_tmpPos - 1);

		if(!l_contact.empty())
		{
			switch(l_contact[0])
			{
			case '#':
			case '&':
				// channel, l_contact = channel name, all is fine.
				break;
			case '@':
			case '+':
			case '%':
				// onotice or something like that.
				l_contact.erase(0, 1);
				// left in l_contact is the channel name.
				break;
			default:
				// probably a query message. Need to make l_contact the nick name:
				{
					l_tmpPos = l_line.find('!');

					if(l_tmpPos != std::string::npos)
					{
						l_contact = l_line.substr(1, l_tmpPos - 1);
					}
					else
					{
						l_contact.clear();
					}
				}
				// :TODO: for future versions: keep track of local nickname and use that to determine channel/query.
			}
		}
	}

	if(l_contact.empty())
		return NULL;

	if(l_cmd_type == CMD_PRIVMSG && l_message.find("\x01""ACTION ") == 0)
	{
		l_message.erase(0, 8);
		if(l_message.size() > 0 && l_message[l_message.size() - 1] == 0x01) l_message.erase(l_message.size() - 1);
		l_cmd_type = CMD_ACTION;
	}

	if(l_message.find("+OK ") == 0)
		l_message.erase(0, 4);
	else if(l_message.find("mcps ") == 0)
		l_message.erase(0, 5);
	else
		return NULL; // something must have gone awry.

	// account for stuff like trailing time stamps from BNCs:
	if((l_tmpPos = l_message.find(' ')) != std::string::npos)
	{
		l_trailing = l_message.substr(l_tmpPos);
		l_message.erase(l_tmpPos);
	}

	// get blowfish key...
	bool l_cbc;
	std::string l_blowKey, l_networkName;

	::EnterCriticalSection(&s_socketMapLock);
	l_networkName = s_socketMap[a_socket];
	::LeaveCriticalSection(&s_socketMapLock);

	l_blowKey = l_ini->GetBlowKey(l_networkName, l_contact, l_cbc);

	if(l_blowKey.empty())
		return NULL;

	// put together new message:
	std::string l_newMsg;

	if(l_cbc && !l_message.empty() && l_message[0] != '*')
	{
		// silent fallback to old style
		l_cbc = false;
	}
	else if(!l_cbc && !l_message.empty() && l_message[0] == '*')
	{
		// auto-enable new style even for non-prefixed keys
		l_cbc = true;
	}

	if(l_cbc && !l_message.empty() && l_message[0] == '*')
	{
		// strip asterisk
		l_message.erase(0, 1);
	}

	int l_decryptionResult = blowfish_decrypt_auto(l_cbc, l_message, l_newMsg, l_blowKey);

	switch(l_decryptionResult)
	{
	case -1:
		l_newMsg = l_message + "=[FiSH: DECRYPTION FAILED!]=";
		break;
	case 1:
		l_newMsg += "\x02&\x02";
		 /* fall through */
	case 0:
		l_newMsg += l_trailing;
		if(l_ini->GetSectionBool(l_networkName, l_contact, L"mark_encrypted", !l_ini->GetStringW(L"mark_encrypted", L"").empty()))
		{ // try local setting and use global setting as default ^^
			int l_markPos = l_ini->GetInt(L"mark_position"); // 1 = append, 2 = prepend, 0 = disabled
			if(l_markPos > 0 && l_markPos <= 2)
			{
				std::string l_mark =
					SimpleMIRCParser(
						UnicodeToCp(CP_UTF8,
							l_ini->GetStringW(L"mark_encrypted")));

#if 0
				bool l_msgValid = Utf8Validate(l_newMsg.c_str();
				bool l_markValid = Utf8Validate(l_mark.c_str());

				if(!l_msgValid && l_markValid)
				{
					// prevent total message corruption by using
					// "neutral" crypt-mark
					l_mark = " \x0315*\x03";
				}
#endif
				
				if(l_markPos == 1)
					l_newMsg.append(l_mark);
				else
					l_newMsg.insert(0, l_mark);
			}
		}
		break;
	}

	// compatibility fix
	// (we only encode the actual MSG part of CTCP ACTIONs, but the old FiSH.dll and some scripts etc.
	// encode the whole thing including \x01 and so on)
	if(l_cmd_type == CMD_PRIVMSG && l_newMsg.find("\x01""ACTION ") == 0)
	{
		l_newMsg.erase(0, 8);
		if(l_newMsg.size() > 0 && l_newMsg[l_newMsg.size() - 1] == 0x01) l_newMsg.erase(l_newMsg.size() - 1);
		l_cmd_type = CMD_ACTION;
	}

	// form new line:
	l_newMsg = l_line.substr(0, l_msgPos) +
		(l_cmd_type == CMD_ACTION ? "\x01""ACTION " : "") +
		l_leading + l_newMsg +
		(l_cmd_type == CMD_ACTION ? "\x01\n" : "\n");

	// return and let fish_inject handle the rest:
	char* szResult = new char[l_newMsg.size() + 1];
	strcpy_s(szResult, l_newMsg.size() + 1, l_newMsg.c_str());

	return szResult;
}


/* for use from fish_inject.dll */
EXPORT_SIG(__declspec(dllexport) char*) _OnOutgoingIRCLine(HANDLE a_socket, const char* a_line, size_t a_len)
{
	auto l_ini = GetBlowIni();

	if(!l_ini->GetBool(L"process_outgoing", true))
		return NULL;

	/** list of messages we possibly need to encrypt: **
		PRIVMSG #chan :lulz
		PRIVMSG #chan :\x01ACTION says hi\x01
		NOTICE test :y hello there
		TOPIC #chan :new topic
	*/

	enum {
		CMD_PRIVMSG = 1,
		CMD_ACTION,
		CMD_NOTICE,
		CMD_TOPIC
	} l_cmd_type;

	// figure out type of message...
	if(!_strnicmp(a_line, "PRIVMSG ", 8))
		l_cmd_type = CMD_PRIVMSG;
	else if(!_strnicmp(a_line, "NOTICE ", 7))
		l_cmd_type = CMD_NOTICE;
	else if(!_strnicmp(a_line, "TOPIC ", 6))
		l_cmd_type = CMD_TOPIC;
	else
		return NULL;

	// check notice encryption setting:
	if(l_cmd_type == CMD_NOTICE && !l_ini->GetBool(L"encrypt_notice", false))
		return NULL;

	// split line:
	std::string l_line(a_line, a_len);
	std::string::size_type l_targetPos = l_line.find(' ') + 1,
		l_msgPos = l_line.find(" :", l_targetPos);

	if(l_msgPos == std::string::npos)
		return NULL; // "should never happen"

	std::string l_target = l_line.substr(l_targetPos, l_msgPos - l_targetPos),
		l_message = l_line.substr(l_msgPos + 2), l_networkName;

	::EnterCriticalSection(&s_socketMapLock);
	l_networkName = s_socketMap[a_socket];
	::LeaveCriticalSection(&s_socketMapLock);

	// kill trailing whitespace, we'll add back the new line later:
	StrTrimRight(l_message);

	if(l_message.empty())
		return NULL;

	// check topic encryption setting:
	if(l_cmd_type == CMD_TOPIC && !l_ini->GetSectionBool(l_networkName, l_target, L"encrypt_topic", false))
		return NULL;

	// don't encrypt DH1080 key exchange:
	if(l_cmd_type == CMD_NOTICE && l_message.find("DH1080_") == 0)
		return NULL;

	// check for CTCPs:
	if(l_message[0] == 0x01)
	{
		if(l_message.find("\x01""ACTION ") != 0 || !l_ini->GetBool(L"encrypt_action", false))
			return NULL;
		else
		{
			l_message = l_message.substr(8, l_message.size() - 10); // strip trailing \x01 too
			l_cmd_type = CMD_ACTION;
		}
	}

	// get blowfish key...
	bool l_cbc;
	const std::string l_blowKey = l_ini->GetBlowKey(l_networkName, l_target, l_cbc);

	if(l_blowKey.empty())
		return NULL;

	// put together new message:
	std::string l_newMsg;

	// check for plain prefix...
	const std::string l_plainPrefix = l_ini->GetString(L"plain_prefix", L"+p ");
	if(!l_plainPrefix.empty() && l_message.find(l_plainPrefix) == 0)
	{
		l_newMsg = l_line.substr(0, l_msgPos + 2) + l_message.substr(l_plainPrefix.size()) + "\n";
	}
	else
	{
		blowfish_encrypt_auto(l_cbc, l_message, l_newMsg, l_blowKey);

		l_newMsg =
			l_line.substr(0, l_msgPos + 2) +
			(l_cmd_type == CMD_ACTION ? "\x01""ACTION +OK " : "+OK ") +
			std::string(l_cbc ? "*" : "") + l_newMsg +
			(l_cmd_type == CMD_ACTION ? "\x01\n" : "\n");
	}

	char* szResult = new char[l_newMsg.size() + 1];
	strcpy_s(szResult, l_newMsg.size() + 1, l_newMsg.c_str());

	return szResult;
}


/* for use from fish_inject.dll */
EXPORT_SIG(__declspec(dllexport) void) _FreeString(const char* a_str)
{
	if(a_str) delete[] a_str;
}


/* called from fish_inject.dll */
EXPORT_SIG(__declspec(dllexport) void) _OnSocketClosed(HANDLE a_socket)
{
	::EnterCriticalSection(&s_socketMapLock);
	s_socketMap.erase(a_socket);
	::LeaveCriticalSection(&s_socketMapLock);
}


/* for use from MSL */
EXPORT_SIG(int) FiSH_SetIniPath(HWND mWnd, HWND aWnd, char *data, char *parms, BOOL show, BOOL nopause)
{
	if(data && *data)
	{
		auto l_ini = GetBlowIni();

		l_ini->SetIniPath(UnicodeFromCp(CP_UTF8, data));
	}

	return 1;
}

// in: nothing
// out: "%priv_key %pub_key"
EXPORT_SIG(int) DH1080_gen(HWND mWnd, HWND aWnd, char *data, char *parms, BOOL show, BOOL nopause)
{
	std::string l_privKey, l_pubKey;

	if(DH1080_Generate(l_privKey, l_pubKey))
	{
		const std::string l_tmp = l_privKey + " " + l_pubKey;

		strcpy_s(data, 900, l_tmp.c_str());

		return 3;
	}
	else
	{
		return 0;
	}
}


// in: "%priv_key %pub_key"
// out: shared secret
EXPORT_SIG(int) DH1080_comp(HWND mWnd, HWND aWnd, char *data, char *parms, BOOL show, BOOL nopause)
{
	if(data && *data)
	{
		const string_vector l_data = SplitString(data, " ");

		if(l_data.size() >= 2)
		{
			const std::string l_shared = DH1080_Compute(l_data[0], l_data[1]);

			strcpy_s(data, 900, l_shared.c_str());

			return 3;
		}
	}

	return 0;
}


EXPORT_SIG(int) FiSH_WriteKey10(HWND mWnd, HWND aWnd, char *data, char *parms, BOOL show, BOOL nopause)
{
	if(data && *data)
	{
		string_vector l_data = SplitString(data, " ", 4);
		/* <decode_utf8|raw_bytes> <network> <contact> <key> */

		if(l_data.size() == 4)
		{
			if(_stricmp(l_data[0].c_str(), "decode_utf8") == 0)
			{
				// old FiSH keys have always been ANSI encoded.
				l_data[3] = UnicodeToCp(CP_ACP, UnicodeFromCp(CP_UTF8, l_data[3]));
			}

			GetBlowIni()->WriteBlowKey(l_data[1], l_data[2], l_data[3]);

			return 1;
		}
	}

	return 0;
}


EXPORT_SIG(int) FiSH_GetKey10(HWND mWnd, HWND aWnd, char *data, char *parms, BOOL show, BOOL nopause)
{
	if(data && *data)
	{
		string_vector l_data = SplitString(data, " ");
		/* <network> <contact> */

		if(l_data.size() >= 2)
		{
			bool l_cbc;
			std::string l_key = GetBlowIni()->GetBlowKey(l_data[0], l_data[1], l_cbc);

			if(l_cbc)
			{
				// :TODO: find out if there's a better way for UI integration and so forth
				l_key.insert(0, "cbc:");
			}

			strcpy_s(data, 900, l_key.c_str());

			return 3;
		}
	}

	return 1;
}


EXPORT_SIG(int) FiSH_DelKey10(HWND mWnd, HWND aWnd, char *data, char *parms, BOOL show, BOOL nopause)
{
	if(data && *data)
	{
		string_vector l_data = SplitString(data, " ");
		/* <network> <contact> */

		if(l_data.size() >= 2)
		{
			GetBlowIni()->DeleteBlowKey(l_data[0], l_data[1]);
		}
	}

	return 1;
}


EXPORT_SIG(int) FiSH_decrypt_msg(HWND mWnd, HWND aWnd, char *data, char *parms, BOOL show, BOOL nopause)
{
	if(data && *data)
	{
		const string_vector l_data = SplitString(data, " ", 2);

		if(l_data.size() >= 2)
		{
			std::string l_key = l_data[0], l_message = l_data[1];

			if(l_message.find("+OK ") == 0)
				l_message.erase(0, 4);
			else if(l_message.find("mcps ") == 0)
				l_message.erase(0, 5);

			std::string l_decrypted;
			bool l_cbc = HasCBCPrefix(l_key, true);

			if(!l_cbc && !l_message.empty() && l_message[0] == '*')
			{
				l_cbc = true;
				l_message.erase(0, 1);
			}

			int l_result = blowfish_decrypt_auto(l_cbc, l_message, l_decrypted, l_key);

			if(l_result == 1)
			{
				l_decrypted += "&";
			}
			else if(l_result < 0)
			{
				l_decrypted += "=[FiSH: DECRYPTION FAILED!]=";
			}

			// use strncpy so strcpy_s doesn't terminate if l_decrypted.size() > 899
			strncpy_s(data, 900, l_decrypted.c_str(), 899);

			return 3;
		}
	}

	return 0;
}


EXPORT_SIG(int) FiSH_encrypt_msg(HWND mWnd, HWND aWnd, char *data, char *parms, BOOL show, BOOL nopause)
{
	if(data && *data)
	{
		const string_vector l_data = SplitString(data, " ", 2);

		if(l_data.size() >= 2)
		{
			std::string l_key = l_data[0], l_message = l_data[1];
			std::string l_encrypted;

			if(!HasCBCPrefix(l_key, true))
			{
				blowfish_encrypt(l_message, l_encrypted, l_key);
			}
			else
			{
				blowfish_encrypt_cbc(l_message, l_encrypted, l_key);
				l_encrypted.insert(0, "*"); // mark CBC mode "message"
			}

			strncpy_s(data, 900, l_encrypted.c_str(), 899);

			return 3;
		}
	}

	return 0;
}


EXPORT_SIG(int) FiSH_GetMyIP(HWND mWnd, HWND aWnd, char *data, char *parms, BOOL show, BOOL nopause)
{
	auto l_ini = GetBlowIni();
	std::wstring l_lookupUrl = l_ini->GetStringW(L"MyIP_service");

	if(l_lookupUrl.empty())
		return 0;

	if(l_lookupUrl.find(L"http://") != 0 && l_lookupUrl.find(L"https://") != 0)
		l_lookupUrl.insert(0, L"http://");

	const std::string l_response = HttpDownloadTextFile(l_lookupUrl);

	if(!l_response.empty())
	{
		std::string::size_type l_pos = l_response.find_first_of("0123456789");

		while(l_pos != std::string::npos)
		{
			std::string l_buf;

			while(l_pos < l_response.size() && (isdigit(l_response[l_pos]) || l_response[l_pos] == '.'))
			{
				l_buf += l_response[l_pos];
				l_pos++;
			}

			unsigned short ip[4];

			if(l_buf.size() >= 7 && sscanf_s(l_buf.c_str(), "%hu.%hu.%hu.%hu", &ip[0], &ip[1], &ip[2], &ip[3]) == 4)
			{
				if((ip[0] > 0) && (ip[0] < 255) && (ip[1] < 255) && (ip[2] < 255) && (ip[3] < 255))
				{
					strcpy_s(data, 900, l_buf.c_str());
					return 3;
				}
			}

			l_pos = l_response.find_first_of("0123456789", l_pos + 1);
		}
	}

	return 1;
}


/* dummy call to show compililation date */

extern "C" int __stdcall _callMe(HWND mWnd, HWND aWnd, char *data, char *parms, BOOL show, BOOL nopause)
{
	strcpy_s(data, 900, "/echo -a *** FiSH 10 *** by [c&f] *** fish_10.dll\xA0\xA0\xA0\xA0\xA0""compiled " __DATE__ " " __TIME__ " ***");
	return 2;
}


/* DllMain for initialization purposes */

BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID lpvReserved)
{
	switch(fdwReason)
	{
	case DLL_PROCESS_ATTACH:
		::InitializeCriticalSection(&s_iniLock);
		::InitializeCriticalSection(&s_socketMapLock);
		break;
	case DLL_PROCESS_DETACH:
		::DeleteCriticalSection(&s_iniLock);
		::DeleteCriticalSection(&s_socketMapLock);
		break;
	}

	return TRUE;
}

