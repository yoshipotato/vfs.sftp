# VFS SFTP addon for Kodi

## Fork changes (WinINET host + reconnects)

- **Host from the Windows proxy setting.** On desktop Windows the add-on reads the
  WinINET proxy (Settings > Network > Proxy, "Use a proxy server") and connects to that
  host instead of the one in the Kodi source. Port, user and password still come from
  the source. With no proxy enabled (or on other platforms) the source's host is used.
  A changed proxy IP is picked up on the next request.
- **Connect retries.** 3 attempts per connect, 5 s timeout each, 1 s then 2 s between
  them. Wrong credentials or a changed host key are not retried.
- **Auto-reconnect.** A dropped or dead session is reconnected on the next request
  (after a failed connect it fails fast for 5 s so Kodi doesn't hang). Every operation
  is retried once after a reconnect, and open files are reopened at the same position,
  so playback can continue after a short network drop.
- **Operation timeout.** 15 s for blocking SFTP operations once connected.

The values are constants at the top of `src/SFTPSession.cpp`.


This is a [Kodi](https://kodi.tv) VFS addon to support SFTP file systems.

[![License: GPL-2.0-or-later](https://img.shields.io/badge/License-GPL%20v2+-blue.svg)](LICENSE.md)
[![Build and run tests](https://github.com/xbmc/vfs.sftp/actions/workflows/build.yml/badge.svg?branch=Nexus)](https://github.com/xbmc/vfs.sftp/actions/workflows/build.yml)
[![Build Status](https://dev.azure.com/teamkodi/binary-addons/_apis/build/status/xbmc.vfs.sftp?branchName=Nexus)](https://dev.azure.com/teamkodi/binary-addons/_build/latest?definitionId=54&branchName=Nexus)
[![Build Status](https://jenkins.kodi.tv/view/Addons/job/xbmc/job/vfs.sftp/job/Nexus/badge/icon)](https://jenkins.kodi.tv/blue/organizations/jenkins/xbmc%2Fvfs.sftp/branches/)
<!--- [![Build Status](https://ci.appveyor.com/api/projects/status/github/xbmc/vfs.sftp?svg=true)](https://ci.appveyor.com/project/xbmc/vfs-sftp) -->

## Build instructions

When building the addon you have to use the correct branch depending on which version of Kodi you're building against.
If you want to build the addon to be compatible with the latest kodi `master` commit, you need to checkout the branch with the current kodi codename.
Also make sure you follow this README from the branch in question.

### Linux

The following instructions assume you will have built Kodi already in the `kodi-build` directory 
suggested by the README.

1. `git clone --branch master https://github.com/xbmc/xbmc.git kodi`
2. `git clone --branch Matrix https://github.com/xbmc/vfs.sftp.git`
3. `cd vfs.sftp && mkdir build && cd build`
4. `cmake -DADDONS_TO_BUILD=vfs.sftp -DADDON_SRC_PREFIX=../.. -DCMAKE_BUILD_TYPE=Debug -DCMAKE_INSTALL_PREFIX=../../kodi/kodi-build/addons -DPACKAGE_ZIP=1 ../../kodi/cmake/addons`
5. `make`

The addon files will be placed in `../../kodi/kodi-build/addons` so if you build Kodi from source and run it directly 
the addon will be available as a system addon.
