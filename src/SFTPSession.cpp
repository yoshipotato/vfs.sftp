/*
 *  Copyright (C) 2005-2021 Team Kodi (https://kodi.tv)
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSE.md for more information.
 */

#include "SFTPSession.h"
#include "WinINetProxy.h"

#include <fcntl.h>
#include <thread>
#include <kodi/General.h>
#include <sstream>
#include <sys/stat.h>
#include <sys/types.h>

// Timeouts and retries
static constexpr long SFTP_CONNECT_TIMEOUT_SEC = 5; // per connection attempt
static constexpr long SFTP_IO_TIMEOUT_SEC = 15; // blocking SFTP operations once connected
static constexpr int SFTP_CONNECT_ATTEMPTS = 3; // connection attempts before giving up
static constexpr int SFTP_RETRY_BASE_DELAY_MS = 1000; // delay between attempts, doubles each time
static constexpr int SFTP_RECONNECT_COOLDOWN_SEC = 5; // after a failed connect, fail fast for this long
#ifndef S_ISREG
#define S_ISREG(m) ((m & _S_IFREG) != 0)
#endif
#ifndef S_ISDIR
#define S_ISDIR(m) ((((m)) & 0170000) == (0040000))
#endif
#ifndef S_ISLNK
#define S_ISLNK(m) ((((m)) & 0170000) == (0120000))
#endif
#ifndef S_IWUSR
#define S_IWUSR 00200
#endif
#ifndef S_IRUSR
#define S_IRUSR 00400
#endif
#ifndef S_IRWXU
#define S_IRWXU 00700
#endif


static std::string CorrectPath(const std::string& path)
{
  if (path == "~")
    return "./";
  else if (path.substr(0, 2) == "~/")
    return "./" + path.substr(2);
  else
    return "/" + path;
}

static const char* SFTPErrorText(int sftp_error)
{
  switch (sftp_error)
  {
    case SSH_FX_OK:
      return "No error";
    case SSH_FX_EOF:
      return "End-of-file encountered";
    case SSH_FX_NO_SUCH_FILE:
      return "File doesn't exist";
    case SSH_FX_PERMISSION_DENIED:
      return "Permission denied";
    case SSH_FX_BAD_MESSAGE:
      return "Garbage received from server";
    case SSH_FX_NO_CONNECTION:
      return "No connection has been set up";
    case SSH_FX_CONNECTION_LOST:
      return "There was a connection, but we lost it";
    case SSH_FX_OP_UNSUPPORTED:
      return "Operation not supported by the server";
    case SSH_FX_INVALID_HANDLE:
      return "Invalid file handle";
    case SSH_FX_NO_SUCH_PATH:
      return "No such file or directory path exists";
    case SSH_FX_FILE_ALREADY_EXISTS:
      return "An attempt to create an already existing file or directory has been made";
    case SSH_FX_WRITE_PROTECT:
      return "We are trying to write on a write-protected filesystem";
    case SSH_FX_NO_MEDIA:
      return "No media in remote drive";
    case -1:
      return "Not a valid error code, probably called on an invalid session";
    default:
      kodi::Log(ADDON_LOG_ERROR, "SFTPErrorText: Unknown error code: %d", sftp_error);
  }
  return "Unknown error code";
}

CSFTPSession::CSFTPSession(const kodi::addon::VFSUrl& url, const std::string& host)
  : m_host(host),
    m_port(url.GetPort() != 0 ? url.GetPort() : 22),
    m_username(url.GetUsername()),
    m_password(url.GetPassword())
{
  kodi::Log(ADDON_LOG_INFO, "SFTPSession: Creating new session on host '%s:%u' with user '%s'",
            m_host.c_str(), m_port, m_username.c_str());
  std::unique_lock<std::recursive_mutex> lock(m_lock);
  Connect();

  m_LastActive = std::chrono::high_resolution_clock::now();
}

CSFTPSession::~CSFTPSession()
{
  std::unique_lock<std::recursive_mutex> lock(m_lock);
  Disconnect();
}

sftp_file CSFTPSession::CreateFileHande(const std::string& file,
                                        mode_t mode,
                                        unsigned int* generation)
{
  if (EnsureConnected())
  {
    std::unique_lock<std::recursive_mutex> lock(m_lock);
    m_LastActive = std::chrono::high_resolution_clock::now();
    sftp_file handle = sftp_open(m_sftp_session, CorrectPath(file).c_str(), mode, S_IRUSR | S_IWUSR);
    if (!handle && IsConnectionError())
    {
      kodi::Log(ADDON_LOG_WARNING, "SFTPSession: Connection lost opening '%s', reconnecting",
                file.c_str());
      if (Reconnect())
        handle = sftp_open(m_sftp_session, CorrectPath(file).c_str(), mode, S_IRUSR | S_IWUSR);
    }
    if (handle)
    {
      sftp_file_set_blocking(handle);
      if (generation)
        *generation = m_generation;
      return handle;
    }
    else
      kodi::Log(ADDON_LOG_ERROR,
                "SFTPSession: Was connected but couldn't create filehandle for '%s'", file.c_str());
  }
  else
    kodi::Log(ADDON_LOG_ERROR, "SFTPSession: Not connected and can't create file handle for '%s'",
              file.c_str());

  return nullptr;
}

void CSFTPSession::CloseFileHandle(sftp_file handle, unsigned int generation)
{
  std::unique_lock<std::recursive_mutex> lock(m_lock);
  // A handle from an earlier connection points at an SFTP session that has
  // already been freed, so it must not be touched.
  if (handle && generation == m_generation && m_sftp_session)
    sftp_close(handle);
}

bool CSFTPSession::GetDirectory(const std::string& base,
                                const std::string& folder,
                                std::vector<kodi::vfs::CDirEntry>& items)
{
  if (!EnsureConnected())
  {
    kodi::Log(ADDON_LOG_ERROR, "SFTPSession: Not connected, can't list directory '%s'",
              folder.c_str());
    return false;
  }

  bool connectionError = false;
  if (GetDirectoryOnce(base, folder, items, connectionError))
    return true;

  if (connectionError)
  {
    kodi::Log(ADDON_LOG_WARNING, "SFTPSession: Connection lost listing '%s', reconnecting",
              folder.c_str());
    if (Reconnect())
      return GetDirectoryOnce(base, folder, items, connectionError);
  }

  return false;
}

bool CSFTPSession::GetDirectoryOnce(const std::string& base,
                                    const std::string& folder,
                                    std::vector<kodi::vfs::CDirEntry>& items,
                                    bool& connectionError)
{
  int sftp_error = SSH_FX_OK;
  connectionError = false;
  if (m_connected)
  {
    sftp_dir dir = nullptr;

    std::unique_lock<std::recursive_mutex> lock(m_lock);
    m_LastActive = std::chrono::high_resolution_clock::now();
    dir = sftp_opendir(m_sftp_session, CorrectPath(folder).c_str());

    //Doing as little work as possible within the critical section
    if (!dir)
    {
      sftp_error = sftp_get_error(m_sftp_session);
      connectionError = IsConnectionError();
    }

    lock.unlock();

    if (!dir)
    {
      kodi::Log(ADDON_LOG_ERROR, "%s: %s for '%s'", __FUNCTION__, SFTPErrorText(sftp_error),
                folder.c_str());
    }
    else
    {
      bool read = true;
      while (read)
      {
        sftp_attributes attributes = nullptr;

        lock.lock();
        read = sftp_dir_eof(dir) == 0;
        attributes = sftp_readdir(m_sftp_session, dir);
        lock.unlock();

        if (attributes && (attributes->name == nullptr || strcmp(attributes->name, "..") == 0 ||
                           strcmp(attributes->name, ".") == 0))
        {
          lock.lock();
          sftp_attributes_free(attributes);
          lock.unlock();
          continue;
        }

        if (attributes)
        {
          std::string itemName = attributes->name;
          std::string localPath = folder;
          localPath.append(itemName);

          if (attributes->type == SSH_FILEXFER_TYPE_SYMLINK)
          {
            lock.lock();
            sftp_attributes_free(attributes);
            attributes = sftp_stat(m_sftp_session, CorrectPath(localPath).c_str());
            lock.unlock();
            if (attributes == nullptr)
              continue;
          }

          kodi::vfs::CDirEntry entry;
          entry.SetLabel(itemName);

          if (itemName[0] == '.')
            entry.AddProperty("file:hidden", "true");

          entry.SetDateTime(attributes->mtime);

          if (attributes->type & SSH_FILEXFER_TYPE_DIRECTORY)
          {
            localPath.append("/");
            entry.SetFolder(true);
            entry.SetSize(0);
          }
          else
            entry.SetSize(attributes->size);

          entry.SetPath(base + localPath);
          items.push_back(entry);

          lock.lock();
          sftp_attributes_free(attributes);
          lock.unlock();
        }
        else
          read = false;
      }

      lock.lock();
      sftp_closedir(dir);
      lock.unlock();

      return true;
    }
  }
  else
    kodi::Log(ADDON_LOG_ERROR, "SFTPSession: Not connected, can't list directory '%s'",
              folder.c_str());

  return false;
}

bool CSFTPSession::DirectoryExists(const std::string& path)
{
  bool exists = false;
  uint32_t permissions = 0;
  exists = GetItemPermissions(path, permissions);
  return exists && S_ISDIR(permissions);
}

bool CSFTPSession::FileExists(const std::string& path)
{
  bool exists = false;
  uint32_t permissions = 0;
  exists = GetItemPermissions(path, permissions);
  return exists && S_ISREG(permissions);
}

sftp_attributes CSFTPSession::StatAttributes(const std::string& path)
{
  if (!EnsureConnected())
    return nullptr;

  std::unique_lock<std::recursive_mutex> lock(m_lock);
  m_LastActive = std::chrono::high_resolution_clock::now();
  sftp_attributes attributes = sftp_stat(m_sftp_session, CorrectPath(path).c_str());
  if (!attributes && IsConnectionError())
  {
    kodi::Log(ADDON_LOG_WARNING, "SFTPSession: Connection lost reading '%s', reconnecting",
              path.c_str());
    if (Reconnect())
      attributes = sftp_stat(m_sftp_session, CorrectPath(path).c_str());
  }
  return attributes;
}

int CSFTPSession::Stat(const std::string& path, kodi::vfs::FileStatus& buffer)
{
  if (EnsureConnected())
  {
    sftp_attributes attributes = StatAttributes(path);

    if (attributes)
    {
      buffer.SetSize(attributes->size);
      buffer.SetModificationTime(attributes->mtime);
      buffer.SetAccessTime(attributes->atime);

      if S_ISDIR (attributes->permissions)
        buffer.SetIsDirectory(true);
      else if S_ISREG (attributes->permissions)
        buffer.SetIsRegular(true);

      sftp_attributes_free(attributes);
      return 0;
    }
    else
    {
      kodi::Log(ADDON_LOG_ERROR, "SFTPSession::Stat - Failed to get attributes for '%s'",
                path.c_str());
      return -1;
    }
  }
  else
  {
    kodi::Log(ADDON_LOG_ERROR, "SFTPSession::Stat - Failed because not connected for '%s'",
              path.c_str());
    return -1;
  }
}

int CSFTPSession::Seek(sftp_file handle, uint64_t position)
{
  std::unique_lock<std::recursive_mutex> lock(m_lock);
  m_LastActive = std::chrono::high_resolution_clock::now();
  int result = sftp_seek64(handle, position);
  return result;
}

int CSFTPSession::Read(sftp_file handle, void* buffer, size_t length)
{
  std::unique_lock<std::recursive_mutex> lock(m_lock);
  m_LastActive = std::chrono::high_resolution_clock::now();
  int result = sftp_read(handle, buffer, length);
  return result;
}

int CSFTPSession::Write(sftp_file handle, const void* buffer, size_t length)
{
  std::unique_lock<std::recursive_mutex> lock(m_lock);
  m_LastActive = std::chrono::high_resolution_clock::now();
  int result = sftp_write(handle, buffer, length);
  return result;
}

int64_t CSFTPSession::GetPosition(sftp_file handle)
{
  std::unique_lock<std::recursive_mutex> lock(m_lock);
  m_LastActive = std::chrono::high_resolution_clock::now();
  int64_t result = sftp_tell64(handle);
  return result;
}

bool CSFTPSession::IsIdle()
{
  std::chrono::high_resolution_clock::time_point now = std::chrono::high_resolution_clock::now();
  return static_cast<int>(
             std::chrono::duration_cast<std::chrono::milliseconds>(now - m_LastActive).count()) >
         90000;
}

bool CSFTPSession::DeleteFile(const std::string& path)
{
  return RunWithRetry([&]() { return sftp_unlink(m_sftp_session, CorrectPath(path).c_str()); });
}

bool CSFTPSession::DeleteDirectory(const std::string& path)
{
  return RunWithRetry([&]() { return sftp_rmdir(m_sftp_session, CorrectPath(path).c_str()); });
}

bool CSFTPSession::MakeDirectory(const std::string& path)
{
  return RunWithRetry([&]() { return sftp_mkdir(m_sftp_session, CorrectPath(path).c_str(), S_IRWXU); });
}

bool CSFTPSession::RenameFile(const std::string& path_from, const std::string& path_to)
{
  return RunWithRetry([&]() {
    return sftp_rename(m_sftp_session, CorrectPath(path_from).c_str(),
                       CorrectPath(path_to).c_str());
  });
}

bool CSFTPSession::VerifyKnownHost(ssh_session session)
{
#if !(LIBSSH_VERSION_MAJOR == 0 && LIBSSH_VERSION_MINOR < 8)
  // Code used on libssh 0.8.0 and above
  // See https://api.libssh.org/stable/deprecated.html
  switch (ssh_session_is_known_server(session))
  {
    case SSH_KNOWN_HOSTS_OK:
      return true;
    case SSH_KNOWN_HOSTS_CHANGED:
      kodi::Log(ADDON_LOG_ERROR, "SFTPSession: Server that was known has changed");
      return false;
    case SSH_KNOWN_HOSTS_OTHER:
      kodi::Log(ADDON_LOG_ERROR,
                "SFTPSession: The host key for this server was not found but an other type of key "
                "exists. An attacker might change the default server key to confuse your client "
                "into thinking the key does not exist");
      return false;
    case SSH_KNOWN_HOSTS_NOT_FOUND:
      kodi::Log(ADDON_LOG_INFO, "SFTPSession: Server file was not found, creating a new one");
    case SSH_KNOWN_HOSTS_UNKNOWN:
      kodi::Log(ADDON_LOG_INFO, "SFTPSession: Server unkown, we trust it for now");
      if (ssh_session_update_known_hosts(session) != SSH_OK)
      {
        kodi::Log(ADDON_LOG_ERROR, "CSFTPSession: Failed to save host '%s'", strerror(errno));
        return false;
      }

      return true;
    case SSH_KNOWN_HOSTS_ERROR:
      kodi::Log(ADDON_LOG_ERROR, "SFTPSession: Failed to verify host '%s'", ssh_get_error(session));
      return false;
  }
#else
  switch (ssh_is_server_known(session))
  {
    case SSH_SERVER_KNOWN_OK:
      return true;
    case SSH_SERVER_KNOWN_CHANGED:
      kodi::Log(ADDON_LOG_ERROR, "SFTPSession: Server that was known has changed");
      return false;
    case SSH_SERVER_FOUND_OTHER:
      kodi::Log(ADDON_LOG_ERROR,
                "SFTPSession: The host key for this server was not found but an other type of key "
                "exists. An attacker might change the default server key to confuse your client "
                "into thinking the key does not exist");
      return false;
    case SSH_SERVER_FILE_NOT_FOUND:
      kodi::Log(ADDON_LOG_INFO, "SFTPSession: Server file was not found, creating a new one");
    case SSH_SERVER_NOT_KNOWN:
      kodi::Log(ADDON_LOG_INFO, "SFTPSession: Server unkown, we trust it for now");
      if (ssh_write_knownhost(session) < 0)
      {
        kodi::Log(ADDON_LOG_ERROR, "CSFTPSession: Failed to save host '%s'", strerror(errno));
        return false;
      }

      return true;
    case SSH_SERVER_ERROR:
      kodi::Log(ADDON_LOG_ERROR, "SFTPSession: Failed to verify host '%s'", ssh_get_error(session));
      return false;
  }
#endif

  return false;
}

bool CSFTPSession::Connect()
{
  int delayMs = SFTP_RETRY_BASE_DELAY_MS;
  for (int attempt = 1; attempt <= SFTP_CONNECT_ATTEMPTS; ++attempt)
  {
    ConnectResult result = ConnectOnce();
    if (result == ConnectResult::OK)
    {
      if (attempt > 1)
        kodi::Log(ADDON_LOG_INFO, "SFTPSession: Connected to '%s:%u' on attempt %d",
                  m_host.c_str(), m_port, attempt);
      return true;
    }

    Disconnect();

    if (result == ConnectResult::FATAL)
      break;

    if (attempt < SFTP_CONNECT_ATTEMPTS)
    {
      kodi::Log(ADDON_LOG_WARNING,
                "SFTPSession: Connection attempt %d/%d to '%s:%u' failed, retrying in %d ms",
                attempt, SFTP_CONNECT_ATTEMPTS, m_host.c_str(), m_port, delayMs);
      std::this_thread::sleep_for(std::chrono::milliseconds(delayMs));
      delayMs *= 2;
    }
  }

  kodi::Log(ADDON_LOG_ERROR, "SFTPSession: Giving up connecting to '%s:%u'", m_host.c_str(),
            m_port);
  m_lastConnectAttempt = std::chrono::steady_clock::now();
  return false;
}

CSFTPSession::ConnectResult CSFTPSession::ConnectOnce()
{
  long timeout = SFTP_CONNECT_TIMEOUT_SEC;
  unsigned int port = m_port;
  m_connected = false;
  m_session = nullptr;
  m_sftp_session = nullptr;

  m_session = ssh_new();
  if (m_session == nullptr)
  {
    kodi::Log(ADDON_LOG_ERROR, "SFTPSession: Failed to initialize session for host '%s'",
              m_host.c_str());
    return ConnectResult::FATAL;
  }

  if (ssh_options_set(m_session, SSH_OPTIONS_USER, m_username.c_str()) < 0)
  {
    kodi::Log(ADDON_LOG_ERROR, "SFTPSession: Failed to set username '%s' for session",
              m_username.c_str());
    return ConnectResult::FATAL;
  }

  if (ssh_options_set(m_session, SSH_OPTIONS_HOST, m_host.c_str()) < 0)
  {
    kodi::Log(ADDON_LOG_ERROR, "SFTPSession: Failed to set host '%s' for session",
              m_host.c_str());
    return ConnectResult::FATAL;
  }

  if (ssh_options_set(m_session, SSH_OPTIONS_PORT, &port) < 0)
  {
    kodi::Log(ADDON_LOG_ERROR, "SFTPSession: Failed to set port '%u' for session", port);
    return ConnectResult::FATAL;
  }

#if defined(TARGET_DARWIN_IOS) || defined(TARGET_WINDOWS_STORE)
  std::string sshFolder = kodi::vfs::TranslateSpecialProtocol("special://home/.ssh");
  if (ssh_options_set(m_session, SSH_OPTIONS_SSH_DIR, sshFolder.c_str()) < 0)
  {
    kodi::Log(ADDON_LOG_ERROR, "SFTPSession: Failed to set .ssh folder to '%s' for session",
              sshFolder.c_str());
    return ConnectResult::FATAL;
  }
#endif

  int verbosity = SSH_LOG_NOLOG;
  ssh_options_set(m_session, SSH_OPTIONS_LOG_VERBOSITY, &verbosity);
  ssh_options_set(m_session, SSH_OPTIONS_TIMEOUT, &timeout);

  if (ssh_connect(m_session))
  {
    kodi::Log(ADDON_LOG_ERROR, "SFTPSession: Failed to connect '%s'", ssh_get_error(m_session));
    return ConnectResult::RETRY;
  }

  if (!VerifyKnownHost(m_session))
  {
    kodi::Log(ADDON_LOG_ERROR, "SFTPSession: Host is not known '%s'", ssh_get_error(m_session));
    return ConnectResult::FATAL;
  }

  int noAuth = SSH_AUTH_DENIED;
  if ((noAuth = ssh_userauth_none(m_session, nullptr)) == SSH_AUTH_ERROR)
  {
    kodi::Log(ADDON_LOG_ERROR, "SFTPSession: Failed to authenticate via guest '%s'",
              ssh_get_error(m_session));
    return ConnectResult::RETRY;
  }

  int method = ssh_userauth_list(m_session, nullptr);

  // Try to authenticate with public key first
  int publicKeyAuth = SSH_AUTH_DENIED;
  if (method & SSH_AUTH_METHOD_PUBLICKEY &&
      (publicKeyAuth = ssh_userauth_publickey_auto(m_session, nullptr, nullptr)) == SSH_AUTH_ERROR)
  {
    kodi::Log(ADDON_LOG_ERROR, "SFTPSession: Failed to authenticate via publickey '%s'",
              ssh_get_error(m_session));
    return ConnectResult::RETRY;
  }

  // Try to authenticate with password
  int passwordAuth = SSH_AUTH_DENIED;
  if (method & SSH_AUTH_METHOD_PASSWORD)
  {
    if (publicKeyAuth != SSH_AUTH_SUCCESS &&
        (passwordAuth = ssh_userauth_password(m_session, m_username.c_str(),
                                              m_password.c_str())) == SSH_AUTH_ERROR)
    {
      kodi::Log(ADDON_LOG_ERROR, "SFTPSession: Failed to authenticate via password '%s'",
                ssh_get_error(m_session));
      return ConnectResult::RETRY;
    }
  }
  else if (!m_password.empty())
  {
    kodi::Log(ADDON_LOG_ERROR,
              "SFTPSession: Password present, but server does not support password authentication");
  }

  if (noAuth == SSH_AUTH_SUCCESS || publicKeyAuth == SSH_AUTH_SUCCESS ||
      passwordAuth == SSH_AUTH_SUCCESS)
  {
    m_sftp_session = sftp_new(m_session);

    if (m_sftp_session == nullptr)
    {
      kodi::Log(ADDON_LOG_ERROR, "SFTPSession: Failed to initialize channel '%s'",
                ssh_get_error(m_session));
      return ConnectResult::RETRY;
    }

    if (sftp_init(m_sftp_session))
    {
      kodi::Log(ADDON_LOG_ERROR, "SFTPSession: Failed to initialize sftp '%s'",
                ssh_get_error(m_session));
      return ConnectResult::RETRY;
    }

    // Longer timeout for normal operations than for the initial connect
    long ioTimeout = SFTP_IO_TIMEOUT_SEC;
    ssh_options_set(m_session, SSH_OPTIONS_TIMEOUT, &ioTimeout);

    m_connected = true;
    m_generation++;
    return ConnectResult::OK;
  }

  // Wrong user/password or key: retrying won't help.
  kodi::Log(ADDON_LOG_ERROR, "SFTPSession: No authentication method successful");
  return ConnectResult::FATAL;
}

void CSFTPSession::Disconnect()
{
  if (m_sftp_session)
  {
    sftp_free(m_sftp_session);
    // Any open file handles belonged to the freed SFTP session.
    m_generation++;
  }

  if (m_session)
  {
    ssh_disconnect(m_session);
    ssh_free(m_session);
  }

  m_sftp_session = nullptr;
  m_session = nullptr;
  m_connected = false;
}

bool CSFTPSession::EnsureConnected()
{
  std::unique_lock<std::recursive_mutex> lock(m_lock);
  if (m_connected && m_session && ssh_is_connected(m_session))
    return true;

  // Don't block every Kodi request on a server that just failed to answer.
  auto sinceLastFailure = std::chrono::steady_clock::now() - m_lastConnectAttempt;
  if (sinceLastFailure < std::chrono::seconds(SFTP_RECONNECT_COOLDOWN_SEC))
    return false;

  if (m_session)
    kodi::Log(ADDON_LOG_WARNING, "SFTPSession: Connection to '%s:%u' was lost, reconnecting",
              m_host.c_str(), m_port);
  Disconnect();
  return Connect();
}

bool CSFTPSession::Reconnect()
{
  std::unique_lock<std::recursive_mutex> lock(m_lock);
  Disconnect();
  return EnsureConnected();
}

bool CSFTPSession::IsConnectionError()
{
  std::unique_lock<std::recursive_mutex> lock(m_lock);
  if (!m_connected || !m_session || !ssh_is_connected(m_session))
    return true;

  if (ssh_get_error_code(m_session) == SSH_FATAL)
    return true;

  if (m_sftp_session)
  {
    int error = sftp_get_error(m_sftp_session);
    if (error == SSH_FX_CONNECTION_LOST || error == SSH_FX_NO_CONNECTION)
      return true;
  }

  return false;
}

bool CSFTPSession::RunWithRetry(const std::function<int()>& op)
{
  if (!EnsureConnected())
    return false;

  std::unique_lock<std::recursive_mutex> lock(m_lock);
  m_LastActive = std::chrono::high_resolution_clock::now();
  int result = op();
  if (result != 0 && IsConnectionError())
  {
    kodi::Log(ADDON_LOG_WARNING, "SFTPSession: Connection lost, reconnecting and retrying");
    if (Reconnect())
      result = op();
  }
  return result == 0;
}

/*!
 \brief Gets POSIX compatible permissions information about the specified file or directory.
 \param path Remote SSH path to the file or directory.
 \param permissions POSIX compatible permissions information for the file or directory (if it exists). i.e. can use macros S_ISDIR() etc.
 \return Returns \e true, if it was possible to get permissions for the file or directory, \e false otherwise.
 */
bool CSFTPSession::GetItemPermissions(const std::string& path, uint32_t& permissions)
{
  bool gotPermissions = false;
  std::unique_lock<std::recursive_mutex> lock(m_lock);
  sftp_attributes attributes = StatAttributes(path);
  if (attributes)
  {
    if (attributes->flags & SSH_FILEXFER_ATTR_PERMISSIONS)
    {
      permissions = attributes->permissions;
      gotPermissions = true;
    }

    sftp_attributes_free(attributes);
  }
  return gotPermissions;
}

CSFTPSessionManager& CSFTPSessionManager::Get()
{
  static CSFTPSessionManager instance;

  return instance;
}

CSFTPSessionPtr CSFTPSessionManager::CreateSession(const kodi::addon::VFSUrl& url)
{
  // Check if url contains port, else fallback to 22 (default)
  unsigned int port = url.GetPort();
  if (port == 0)
    port = 22;

  // Connect to the host of the Windows (WinINET) proxy when one is enabled,
  // otherwise to the host in the URL. Looked up on every request so a changed
  // proxy IP gets a new session.
  std::string host = GetWinINetProxyHost();
  if (host.empty())
    host = url.GetHostname();

  std::unique_lock<std::recursive_mutex> lock(m_lock);
  std::string key = url.GetUsername() + ":" + url.GetPassword() + "@" + host + ":" +
                    std::to_string(port);
  CSFTPSessionPtr ptr = sessions[key];
  if (ptr == nullptr)
  {
    if (host != url.GetHostname())
      kodi::Log(ADDON_LOG_INFO, "SFTPSession: Using WinINET proxy host '%s' instead of '%s'",
                host.c_str(), url.GetHostname().c_str());
    ptr = CSFTPSessionPtr(new CSFTPSession(url, host));
    sessions[key] = ptr;
  }
  else
  {
    // A cached session that failed or dropped gets another chance (rate limited).
    ptr->EnsureConnected();
  }

  return ptr;
}

void CSFTPSessionManager::ClearOutIdleSessions()
{
  std::unique_lock<std::recursive_mutex> lock(m_lock);
  for (std::map<std::string, CSFTPSessionPtr>::iterator iter = sessions.begin();
       iter != sessions.end();)
  {
    if (iter->second->IsIdle())
      sessions.erase(iter++);
    else
      iter++;
  }
}

void CSFTPSessionManager::DisconnectAllSessions()
{
  std::unique_lock<std::recursive_mutex> lock(m_lock);
  sessions.clear();
}
