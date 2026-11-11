/*
 *  Copyright (C) 2005-2021 Team Kodi (https://kodi.tv)
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSE.md for more information.
 */

#pragma once

#include <atomic>
#include <chrono>
#include <functional>
#include <kodi/addon-instance/VFS.h>
#include <libssh/libssh.h>
#include <libssh/sftp.h>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

class CSFTPSession
{
public:
  /*!
   \param url  Kodi URL (user, password, port)
   \param host Host actually connected to (the WinINET proxy host when one is set,
               otherwise the URL hostname)
   */
  CSFTPSession(const kodi::addon::VFSUrl& url, const std::string& host);
  virtual ~CSFTPSession();

  //! Reconnects if the session is down (rate limited). Returns true when connected.
  bool EnsureConnected();
  //! Drops the current connection and connects again (with retries).
  bool Reconnect();
  //! True when the last failure looks like a dropped/timed out connection.
  bool IsConnectionError();
  //! Bumped on every (re)connect. File handles from an older generation are invalid.
  unsigned int Generation() const { return m_generation; }

  sftp_file CreateFileHande(const std::string& file,
                            mode_t mode,
                            unsigned int* generation = nullptr);
  //! Closes the handle, but only if it belongs to the current connection generation.
  void CloseFileHandle(sftp_file handle, unsigned int generation);
  bool GetDirectory(const std::string& base,
                    const std::string& folder,
                    std::vector<kodi::vfs::CDirEntry>& items);
  bool DirectoryExists(const std::string& path);
  bool FileExists(const std::string& path);
  int Stat(const std::string& path, kodi::vfs::FileStatus& buffer);
  int Seek(sftp_file handle, uint64_t position);
  int Read(sftp_file handle, void* buffer, size_t length);
  int Write(sftp_file handle, const void* buffer, size_t length);
  int64_t GetPosition(sftp_file handle);
  bool IsIdle();
  bool DeleteFile(const std::string& path);
  bool DeleteDirectory(const std::string& path);
  bool MakeDirectory(const std::string& path);
  bool RenameFile(const std::string& path_from, const std::string& path_to);

private:
  bool VerifyKnownHost(ssh_session session);
  enum class ConnectResult
  {
    OK,
    RETRY, // network/timeout problem, worth trying again
    FATAL // bad credentials, host key mismatch etc., retrying won't help
  };
  bool Connect();
  ConnectResult ConnectOnce();
  bool RunWithRetry(const std::function<int()>& op);
  void Disconnect();
  bool GetItemPermissions(const std::string& path, uint32_t& permissions);
  sftp_attributes StatAttributes(const std::string& path);
  bool GetDirectoryOnce(const std::string& base,
                        const std::string& folder,
                        std::vector<kodi::vfs::CDirEntry>& items,
                        bool& connectionError);
  std::recursive_mutex m_lock;

  std::string m_host;
  unsigned int m_port;
  std::string m_username;
  std::string m_password;

  std::atomic<unsigned int> m_generation{0};
  std::chrono::steady_clock::time_point m_lastConnectAttempt;

  bool m_connected = false;
  ssh_session m_session = nullptr;
  sftp_session m_sftp_session = nullptr;
  std::chrono::high_resolution_clock::time_point m_LastActive;
};

typedef std::shared_ptr<CSFTPSession> CSFTPSessionPtr;

class CSFTPSessionManager
{
public:
  static CSFTPSessionManager& Get();
  CSFTPSessionPtr CreateSession(const kodi::addon::VFSUrl& url);
  void ClearOutIdleSessions();
  void DisconnectAllSessions();

private:
  CSFTPSessionManager() {}
  CSFTPSessionManager& operator=(const CSFTPSessionManager&);
  std::recursive_mutex m_lock;
  std::map<std::string, CSFTPSessionPtr> sessions;
};
