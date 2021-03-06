/*
 * Copyright 2005 - 2016 Zarafa and its licensors
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License, version 3,
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#include <kopano/platform.h>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <cctype>
#include <algorithm>
#include <string>
#include <utility>
#include <mapi.h>
#include <mapix.h>
#include <mapicode.h>
#include <mapidefs.h>
#include <mapiutil.h>
#include <inetmapi/inetmapi.h>
#include <kopano/mapiext.h>

#include <kopano/CommonUtil.h>
#include <kopano/MAPIErrors.h>
#include <fileutil.h>
#include <kopano/ECTags.h>
#include <kopano/ECChannel.h>
#include "LMTP.h"
#include <kopano/stringutil.h>
#include "fileutil.h"

using std::string;

LMTP::LMTP(ECChannel *lpChan, const char *szServerPath, ECConfig *lpConf)
{
    m_lpChannel = lpChan;
    m_lpConfig = lpConf;
    m_strPath = szServerPath;
}

/** 
 * Tests the start of the input for the LMTP command. LMTP is case
 * insensitive.
 * LMTP commands are:
 * @arg LHLO
 * @arg MAIL FROM:
 * @arg RCPT TO:
 * @arg DATA
 * @arg RSET
 * @arg QUIT
 * 
 * @param[in]  strCommand The received line from the LMTP client
 * @param[out] eCommand enum describing the received command
 * 
 * @return MAPI error code
 * @retval MAPI_E_CALL_FAILED unknown or unsupported command received
 */
HRESULT LMTP::HrGetCommand(const string &strCommand, LMTP_Command &eCommand)
{
	HRESULT hr = hrSuccess;
	
	if (strncasecmp(strCommand.c_str(), "LHLO", strlen("LHLO")) == 0)
		eCommand = LMTP_Command_LHLO;
	else if (strncasecmp(strCommand.c_str(), "MAIL FROM:", strlen("MAIL FROM:")) == 0)
		eCommand = LMTP_Command_MAIL_FROM;
	else if (strncasecmp(strCommand.c_str(), "RCPT TO:", strlen("RCPT TO:")) == 0)
		eCommand = LMTP_Command_RCPT_TO;
	else if (strncasecmp(strCommand.c_str(), "DATA", strlen("DATA")) == 0)
		eCommand = LMTP_Command_DATA;
	else if (strncasecmp(strCommand.c_str(), "RSET", strlen("RSET")) == 0)
		eCommand = LMTP_Command_RSET;
	else if (strncasecmp(strCommand.c_str(), "QUIT", strlen("QUIT")) == 0)
		eCommand = LMTP_Command_QUIT;
	else
		hr = MAPI_E_CALL_FAILED;
	
	return hr;
}

/** 
 * Send the following response to the LMTP client.
 * 
 * @param[in] strResponse String to send
 * 
 * @return Possible error during write to the client
 */
HRESULT LMTP::HrResponse(const string &strResponse)
{
	HRESULT hr;

	ec_log_debug("< %s", strResponse.c_str());
	hr = m_lpChannel->HrWriteLine(strResponse);
	if (hr != hrSuccess)
		ec_log_err("LMTP write error: %s (%x)",
			GetMAPIErrorMessage(hr), hr);

	return hr;
}

/** 
 * Parse the received string for a valid LHLO command.
 * 
 * @param[in] strInput the full LHLO command received
 * 
 * @return always hrSuccess
 */
HRESULT LMTP::HrCommandLHLO(const string &strInput, string & nameOut)
{
	size_t pos = strInput.find(' ');
	nameOut.assign(strInput.c_str() + pos + 1);

	// Input definitly starts with LHLO
	// use HrResponse("501 5.5.4 Syntax: LHLO hostname"); in case of error, but we don't.
	ec_log_debug("LHLO ID: %s", nameOut.c_str());
	return hrSuccess;
}

/** 
 * Parse the received string for a valid MAIL FROM: command.
 * The correct syntax for the MAIL FROM is:
 *  MAIL FROM:<email@address.domain>
 *
 * However, it's possible extra spaces are added in the string, and we
 * should correctly accept this to deliver the mail.
 * We ignore the contents from the address, and use the From: header.
 * 
 * @param[in] strFrom the full MAIL FROM command
 * 
 * @return MAPI error code
 * @retval MAPI_E_NOT_FOUND < or > character was not found: this is fatal.
 */
HRESULT LMTP::HrCommandMAILFROM(const string &strFrom, std::string *const strAddress)
{
	// strFrom is only checked for syntax
	return HrParseAddress(strFrom, strAddress);
}

/** 
 * Parse the received string for a valid RCPT TO: command.
 * 
 * @param[in]  strTo the full RCPT TO command
 * @param[out] strUnresolved the parsed email address from the command, user will be resolved by DAgent.
 * 
 * @return MAPI error code
 * @retval MAPI_E_NOT_FOUND < or > character was not found: this is fatal.
 */
HRESULT LMTP::HrCommandRCPTTO(const string &strTo, string *strUnresolved)
{
	HRESULT hr = HrParseAddress(strTo, strUnresolved);
	if (hr == hrSuccess)
		ec_log_debug("Resolved command \"%s\" to recipient address \"%s\"",
			strTo.c_str(), strUnresolved->c_str());
	else
		ec_log_err("Invalid recipient address in command \"%s\": %s (%x)",
			strTo.c_str(), GetMAPIErrorMessage(hr), hr);
	return hr;
}

/** 
 * Receive the DATA from the client and save to a file using \r\n
 * enters. This file will be mmap()ed by the DAgent.
 * 
 * @param[in] tmp a FILE pointer to a temporary file with write access
 * 
 * @return MAPI error code, read/write errors from client.
 */
HRESULT LMTP::HrCommandDATA(FILE *tmp)
{
	HRESULT hr;
	std::string inBuffer;
	std::string message;
	int offset;
	ssize_t ret, to_write;

	hr = HrResponse("354 2.1.5 Start mail input; end with <CRLF>.<CRLF>");
	if (hr != hrSuccess) {
		ec_log_err("Error during DATA communication with client: %s (%x).",
			GetMAPIErrorMessage(hr), hr);
		return hr;
	}

	// Now the mail body needs to be read line by line until <CRLF>.<CRLF> is encountered
	while (1) {
		hr = m_lpChannel->HrReadLine(&inBuffer);
		if (hr != hrSuccess) {
			ec_log_err("Error during DATA communication with client: %s (%x).",
				GetMAPIErrorMessage(hr), hr);
			return hr;
		}

			if (inBuffer == ".")
				break;

		offset = 0;
		if (inBuffer[0] == '.')
			offset = 1;			// "remove" escape point, since it wasn't the end of mail marker

		to_write = inBuffer.size() - offset;
		ret = fwrite((char *)inBuffer.c_str() + offset, 1, to_write, tmp);
		if (ret != to_write) {
			ec_log_err("Error during DATA communication with client: %s", strerror(errno));
			return MAPI_E_FAILURE;
		}

		// The data from HrReadLine does not contain the CRLF, so add that here
		if (fwrite("\r\n", 1, 2, tmp) != 2) {
			ec_log_err("Error during DATA communication with client: %s", strerror(errno));
			return MAPI_E_FAILURE;
		}

		message += inBuffer + "\r\n";
	}
#if 0
	if (m_lpLogger->Log(EC_LOGLEVEL_DEBUG + 1)) // really hidden output (limited to 10k in logger)
			m_lpLogger->Log(EC_LOGLEVEL_DEBUG, "Received message:\n" + message);
#endif
	return hrSuccess;
}

/** 
 * Handle the very difficult QUIT command.
 * 
 * @return always hrSuccess
 */
HRESULT LMTP::HrCommandQUIT()
{
	return hrSuccess;
}

/** 
 * Parse an address given in a MAIL FROM or RCPT TO command.
 * 
 * @param[in]  strInput a full MAIL FROM or RCPT TO command
 * @param[out] strAddress the address found in the command
 * 
 * @return MAPI error code
 * @retval MAPI_E_NOT_FOUND mandatory < or > not found in command.
 */
HRESULT LMTP::HrParseAddress(const std::string &strInput, std::string *strAddress)
{
	std::string strAddr;
	size_t pos1;
	size_t pos2;

	pos1 = strInput.find('<');
	pos2 = strInput.find('>', pos1);

	if (pos1 == std::string::npos || pos2 == std::string::npos)
		return MAPI_E_NOT_FOUND;

	strAddr = strInput.substr(pos1+1, pos2-pos1-1);

	trim(strAddr);
	*strAddress = std::move(strAddr);
	return hrSuccess;
}
