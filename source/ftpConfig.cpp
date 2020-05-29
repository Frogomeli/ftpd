// ftpd is a server implementation based on the following:
// - RFC  959 (https://tools.ietf.org/html/rfc959)
// - RFC 3659 (https://tools.ietf.org/html/rfc3659)
// - suggested implementation details from https://cr.yp.to/ftp/filesystem.html
// - Deflate transmission mode for FTP
//   (https://tools.ietf.org/html/draft-preston-ftpext-deflate-04)
//
// Copyright (C) 2020 Michael Theall
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <https://www.gnu.org/licenses/>.

#include "ftpConfig.h"

#include "fs.h"
#include "log.h"

#include <zlib.h>

#include <sys/stat.h>

#include <limits>

namespace
{
constexpr std::uint16_t DEFAULT_PORT = 5000;
constexpr int DEFAULT_DEFLATE_LEVEL  = 6;

bool mkdirParent (std::string const &path_)
{
	auto pos = path_.find_first_of ('/');
	while (pos != std::string::npos)
	{
		auto const dir = path_.substr (0, pos);

		struct stat st;
		auto const rc = ::stat (dir.c_str (), &st);
		if (rc < 0 && errno != ENOENT)
			return false;

		if (rc < 0 && errno == ENOENT)
		{
			auto const rc = ::mkdir (dir.c_str (), 0755);
			if (rc < 0)
				return false;
		}

		pos = path_.find_first_of ('/', pos + 1);
	}

	return true;
}

std::string strip (std::string const &str_)
{
	auto const start = str_.find_first_not_of (" \t");
	if (start == std::string::npos)
		return {};

	auto const end = str_.find_last_not_of (" \t");

	if (end == std::string::npos)
		return str_.substr (start);

	return str_.substr (start, end + 1 - start);
}

template <typename T>
bool parseInt (T &out_, std::string const &val_)
{
	if (val_.empty ())
	{
		errno = EINVAL;
		return false;
	}

	T val = 0;

	for (auto const &c : val_)
	{
		if (!std::isdigit (c))
		{
			errno = EINVAL;
			return false;
		}

		if (std::numeric_limits<T>::max () / 10 < val)
		{
			errno = EOVERFLOW;
			return false;
		}

		val *= 10;

		auto const v = c - '0';
		if (std::numeric_limits<T>::max () - v < val)
		{
			errno = EOVERFLOW;
			return false;
		}

		val += v;
	}

	out_ = val;
	return true;
}
}

///////////////////////////////////////////////////////////////////////////
FtpConfig::~FtpConfig () = default;

FtpConfig::FtpConfig () : m_port (DEFAULT_PORT), m_deflateLevel (DEFAULT_DEFLATE_LEVEL)
{
}

UniqueFtpConfig FtpConfig::create ()
{
	return UniqueFtpConfig (new FtpConfig ());
}

UniqueFtpConfig FtpConfig::load (char const *const path_)
{
	auto config = create ();

	auto fp = fs::File ();
	if (!fp.open (path_))
		return config;

	std::uint16_t port = DEFAULT_PORT;
	int deflateLevel   = DEFAULT_DEFLATE_LEVEL;

	std::string line;
	while (!(line = fp.readLine ()).empty ())
	{
		auto const pos = line.find_first_of ('=');
		if (pos == std::string::npos)
		{
			error ("Ignoring '%s'\n", line.c_str ());
			continue;
		}

		auto const key = strip (line.substr (0, pos));
		auto const val = strip (line.substr (pos + 1));
		if (key.empty () || val.empty ())
		{
			error ("Ignoring '%s'\n", line.c_str ());
			continue;
		}

		if (key == "user")
			config->m_user = val;
		else if (key == "pass")
			config->m_pass = val;
		else if (key == "port")
			parseInt (port, val);
		else if (key == "deflateLevel")
			parseInt (deflateLevel, val);
#ifdef _3DS
		else if (key == "mtime")
		{
			if (val == "0")
				config->m_getMTime = false;
			else if (val == "1")
				config->m_getMTime = true;
			else
				error ("Invalid value for mtime: %s\n", val.c_str ());
		}
#endif
	}

	config->setPort (port);
	config->setDeflateLevel (deflateLevel);

	return config;
}

bool FtpConfig::save (char const *const path_)
{
	if (!mkdirParent (path_))
		return false;

	auto fp = fs::File ();
	if (!fp.open (path_, "wb"))
		return false;

	if (!m_user.empty ())
		std::fprintf (fp, "user=%s\n", m_user.c_str ());
	if (!m_pass.empty ())
		std::fprintf (fp, "pass=%s\n", m_pass.c_str ());
	std::fprintf (fp, "port=%u\n", m_port);
	std::fprintf (fp, "deflateLevel=%u", m_deflateLevel);

#ifdef _3DS
	std::fprintf (fp, "mtime=%u\n", m_getMTime);
#endif

	return true;
}

std::string const &FtpConfig::user () const
{
	return m_user;
}

std::string const &FtpConfig::pass () const
{
	return m_pass;
}

std::uint16_t FtpConfig::port () const
{
	return m_port;
}

int FtpConfig::deflateLevel () const
{
	return m_deflateLevel;
}

#ifdef _3DS
bool FtpConfig::getMTime () const
{
	return m_getMTime;
}
#endif

void FtpConfig::setUser (std::string const &user_)
{
	m_user = user_.substr (0, user_.find_first_of ('\0'));
}

void FtpConfig::setPass (std::string const &pass_)
{
	m_pass = pass_.substr (0, pass_.find_first_of ('\0'));
}

bool FtpConfig::setPort (std::string const &port_)
{
	std::uint16_t parsed;
	if (!parseInt (parsed, port_))
		return false;

	return setPort (parsed);
}

bool FtpConfig::setPort (std::uint16_t const port_)
{
	if (port_ < 1024
#if !defined(NDS) && !defined(_3DS)
	    && port_ != 0
#endif
	)
	{
		errno = EPERM;
		return false;
	}

	m_port = port_;
	return true;
}

bool FtpConfig::setDeflateLevel (std::string const &level_)
{
	int parsed;
	if (!parseInt (parsed, level_))
		return false;

	return setDeflateLevel (parsed);
}

bool FtpConfig::setDeflateLevel (int const level_)
{
	if (level_ < Z_NO_COMPRESSION || level_ > Z_BEST_COMPRESSION)
		return false;

	m_deflateLevel = level_;
	return true;
}

#ifdef _3DS
void FtpConfig::setGetMTime (bool const getMTime_)
{
	m_getMTime = getMTime_;
}
#endif
