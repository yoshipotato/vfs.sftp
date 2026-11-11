/*
 *  Copyright (C) 2005-2021 Team Kodi (https://kodi.tv)
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSE.md for more information.
 */

#include "WinINetProxy.h"

#include <algorithm>
#include <cctype>
#include <vector>

#if defined(_WIN32) && !defined(TARGET_WINDOWS_STORE)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <wininet.h>
#define SFTP_HAVE_WININET 1
#endif

static std::string Trim(const std::string& s)
{
  size_t start = 0;
  size_t end = s.size();
  while (start < end && std::isspace(static_cast<unsigned char>(s[start])))
    start++;
  while (end > start && std::isspace(static_cast<unsigned char>(s[end - 1])))
    end--;
  return s.substr(start, end - start);
}

static std::string HostFromEntry(std::string entry)
{
  entry = Trim(entry);

  // "http=host:port" -> "host:port"
  size_t eq = entry.find('=');
  if (eq != std::string::npos)
    entry = entry.substr(eq + 1);

  // "socks://host:port" -> "host:port"
  size_t scheme = entry.find("://");
  if (scheme != std::string::npos)
    entry = entry.substr(scheme + 3);

  // drop any trailing path
  size_t slash = entry.find('/');
  if (slash != std::string::npos)
    entry = entry.substr(0, slash);

  // "[::1]:8080" -> "::1"
  if (!entry.empty() && entry[0] == '[')
  {
    size_t close = entry.find(']');
    return close == std::string::npos ? std::string() : entry.substr(1, close - 1);
  }

  // "host:port" -> "host" (a single colon means a port, more than one is a bare IPv6 address)
  if (std::count(entry.begin(), entry.end(), ':') == 1)
    entry = entry.substr(0, entry.find(':'));

  return Trim(entry);
}

std::string ParseProxyHost(const std::string& proxyServer)
{
  // Entries are separated by ';' (or whitespace in older settings).
  std::vector<std::string> entries;
  std::string current;
  for (char c : proxyServer)
  {
    if (c == ';' || std::isspace(static_cast<unsigned char>(c)))
    {
      if (!current.empty())
        entries.push_back(current);
      current.clear();
    }
    else
      current += c;
  }
  if (!current.empty())
    entries.push_back(current);

  // Prefer a plain "host:port" entry (one proxy for all protocols), then
  // http/https, then anything else (e.g. socks).
  static const char* preferred[] = {"", "http=", "https=", nullptr};
  for (const char** prefix = preferred; *prefix; ++prefix)
  {
    for (const std::string& e : entries)
    {
      std::string lower = e;
      std::transform(lower.begin(), lower.end(), lower.begin(),
                     [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
      bool match = (**prefix == '\0') ? lower.find('=') == std::string::npos
                                      : lower.rfind(*prefix, 0) == 0;
      if (match)
      {
        std::string host = HostFromEntry(e);
        if (!host.empty())
          return host;
      }
    }
  }

  for (const std::string& e : entries)
  {
    std::string host = HostFromEntry(e);
    if (!host.empty())
      return host;
  }

  return std::string();
}

std::string GetWinINetProxyHost()
{
#ifdef SFTP_HAVE_WININET
  INTERNET_PER_CONN_OPTIONW options[2] = {};
  options[0].dwOption = INTERNET_PER_CONN_FLAGS;
  options[1].dwOption = INTERNET_PER_CONN_PROXY_SERVER;

  INTERNET_PER_CONN_OPTION_LISTW list = {};
  list.dwSize = sizeof(list);
  list.pszConnection = nullptr; // default (LAN) connection settings
  list.dwOptionCount = 2;
  list.pOptions = options;

  DWORD size = sizeof(list);
  if (!InternetQueryOptionW(nullptr, INTERNET_OPTION_PER_CONNECTION_OPTION, &list, &size))
    return std::string();

  std::string proxyServer;
  if (options[1].Value.pszValue)
  {
    const wchar_t* w = options[1].Value.pszValue;
    int len = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (len > 1)
    {
      proxyServer.resize(static_cast<size_t>(len - 1));
      WideCharToMultiByte(CP_UTF8, 0, w, -1, &proxyServer[0], len, nullptr, nullptr);
    }
    GlobalFree(options[1].Value.pszValue);
  }

  // Only use the proxy when "Use a proxy server" is switched on.
  if ((options[0].Value.dwValue & PROXY_TYPE_PROXY) == 0)
    return std::string();

  return ParseProxyHost(proxyServer);
#else
  return std::string();
#endif
}
