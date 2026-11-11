/*
 *  Copyright (C) 2005-2021 Team Kodi (https://kodi.tv)
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSE.md for more information.
 */

#pragma once

#include <string>

/*!
 \brief Returns the host part of the proxy configured in the Windows Internet
        settings (WinINET, the same setting as Internet Options / Settings > Proxy).
 \return The proxy host (IP address or name, without port), or an empty string
         when no proxy is enabled or when not running on desktop Windows.
 */
std::string GetWinINetProxyHost();

/*!
 \brief Extracts the host from a WinINET proxy server string.
        Handles "host:port", "scheme://host:port", "[ipv6]:port" and per-protocol
        lists such as "http=host:port;https=host:port".
 */
std::string ParseProxyHost(const std::string& proxyServer);
