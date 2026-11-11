/*
 *  Copyright (C) 2005-2021 Team Kodi (https://kodi.tv)
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSE.md for more information.
 */


#include <map>
#include <sstream>
#include <fcntl.h>
#include "SFTPSession.h"

// This works around a Windows build bug whereby those functions are defined as macro
#if defined(CreateDirectory)
#undef CreateDirectory
#endif
#if defined(RemoveDirectory)
#undef RemoveDirectory
#endif

#include <kodi/General.h>
#include <kodi/addon-instance/VFS.h>

class ATTR_DLL_LOCAL CSFTPFile : public kodi::addon::CInstanceVFS
{
  struct SFTPContext
  {
    CSFTPSessionPtr session;
    sftp_file sftp_handle = nullptr;
    std::string file;
    mode_t mode = O_RDONLY;
    unsigned int generation = 0; // connection the handle belongs to
    int64_t position = 0; // tracked here so the file can be reopened after a reconnect
  };

public:
  CSFTPFile(const kodi::addon::IInstanceInfo& instance) : CInstanceVFS(instance) {}

  kodi::addon::VFSFileHandle Open(const kodi::addon::VFSUrl& url) override
  {
    return OpenInternal(url, O_RDONLY);
  }

  ssize_t Read(kodi::addon::VFSFileHandle context, uint8_t* buffer, size_t uiBufSize) override
  {
    SFTPContext* ctx = static_cast<SFTPContext*>(context);
    if (!ctx || !ctx->session)
    {
      kodi::Log(ADDON_LOG_ERROR, "SFTPFile: Can't read without a handle");
      return -1;
    }

    // One normal try, plus one more after reconnecting if the connection dropped.
    for (int attempt = 0; attempt < 2; ++attempt)
    {
      if (!EnsureHandle(ctx))
        break;

      int rc = ctx->session->Read(ctx->sftp_handle, buffer, uiBufSize);
      if (rc >= 0)
      {
        ctx->position += rc;
        return rc;
      }

      if (attempt == 0 && ctx->session->IsConnectionError())
      {
        kodi::Log(ADDON_LOG_WARNING, "SFTPFile: Connection lost reading '%s' at %lld, retrying",
                  ctx->file.c_str(), static_cast<long long>(ctx->position));
        ctx->session->Reconnect();
        continue;
      }
      break;
    }

    kodi::Log(ADDON_LOG_ERROR, "SFTPFile: Failed to read %s", ctx->file.c_str());
    return -1;
  }

  ssize_t Write(kodi::addon::VFSFileHandle context, const uint8_t* buffer, size_t uiBufSize) override
  {
    SFTPContext* ctx = static_cast<SFTPContext*>(context);
    if (!ctx || !ctx->session)
    {
      kodi::Log(ADDON_LOG_ERROR, "SFTPFile: Can't write without a handle");
      return -1;
    }

    for (int attempt = 0; attempt < 2; ++attempt)
    {
      if (!EnsureHandle(ctx))
        break;

      int writeBytes = ctx->session->Write(ctx->sftp_handle, buffer, uiBufSize);
      if (writeBytes >= 0)
      {
        ctx->position += writeBytes;
        return writeBytes;
      }

      if (attempt == 0 && ctx->session->IsConnectionError())
      {
        kodi::Log(ADDON_LOG_WARNING, "SFTPFile: Connection lost writing '%s', retrying",
                  ctx->file.c_str());
        ctx->session->Reconnect();
        continue;
      }
      break;
    }

    kodi::Log(ADDON_LOG_ERROR, "SFTPFile: Failed to write %s", ctx->file.c_str());
    return -1;
  }

  int64_t Seek(kodi::addon::VFSFileHandle context, int64_t iFilePosition, int whence) override
  {
    SFTPContext* ctx = static_cast<SFTPContext*>(context);
    if (ctx && ctx->session && EnsureHandle(ctx))
    {
      int64_t position = 0;
      if (whence == SEEK_SET)
        position = iFilePosition;
      else if (whence == SEEK_CUR)
        position = ctx->position + iFilePosition;
      else if (whence == SEEK_END)
        position = GetLength(context) + iFilePosition;

      if (position < 0)
        return -1;

      // sftp_seek64 only moves the local offset, it doesn't talk to the server
      if (ctx->session->Seek(ctx->sftp_handle, static_cast<uint64_t>(position)) == 0)
      {
        ctx->position = position;
        return position;
      }
      else
        return -1;
    }
    else
    {
      kodi::Log(ADDON_LOG_ERROR, "SFTPFile: Can't seek without a handle");
      return -1;
    }
  }

  int64_t GetLength(kodi::addon::VFSFileHandle context) override
  {
    SFTPContext* ctx = static_cast<SFTPContext*>(context);
    kodi::vfs::FileStatus buffer;
    if (ctx->session->Stat(ctx->file.c_str(), buffer) != 0)
      return 0;
    else
      return buffer.GetSize();
  }

  int64_t GetPosition(kodi::addon::VFSFileHandle context) override
  {
    SFTPContext* ctx = static_cast<SFTPContext*>(context);
    if (ctx && ctx->session)
      return ctx->position;

    kodi::Log(ADDON_LOG_ERROR, "SFTPFile: Can't get position without a handle for '%s'",
              ctx->file.c_str());
    return 0;
  }

  bool IoControlGetSeekPossible(kodi::addon::VFSFileHandle context) override { return true; }

  int Stat(const kodi::addon::VFSUrl& url, kodi::vfs::FileStatus& buffer) override
  {
    CSFTPSessionPtr session = CSFTPSessionManager::Get().CreateSession(url);
    if (session)
      return session->Stat(url.GetFilename().c_str(), buffer);
    else
    {
      kodi::Log(ADDON_LOG_ERROR, "SFTPFile: Failed to create session to stat for '%s'",
                url.GetFilename().c_str());
      return -1;
    }
  }

  bool Close(kodi::addon::VFSFileHandle context) override
  {
    SFTPContext* ctx = static_cast<SFTPContext*>(context);
    if (ctx->session && ctx->sftp_handle)
      ctx->session->CloseFileHandle(ctx->sftp_handle, ctx->generation);
    delete ctx;

    return true;
  }

  bool Exists(const kodi::addon::VFSUrl& url) override
  {
    CSFTPSessionPtr session = CSFTPSessionManager::Get().CreateSession(url);
    if (session)
      return session->FileExists(url.GetFilename());
    else
    {
      kodi::Log(ADDON_LOG_ERROR, "SFTPFile: Failed to create session to check exists for '%s'",
                url.GetFilename().c_str());
      return false;
    }
  }

  void ClearOutIdle() override { CSFTPSessionManager::Get().ClearOutIdleSessions(); }

  void DisconnectAll() override { CSFTPSessionManager::Get().DisconnectAllSessions(); }

  bool DirectoryExists(const kodi::addon::VFSUrl& url) override
  {
    CSFTPSessionPtr session = CSFTPSessionManager::Get().CreateSession(url);
    if (session)
      return session->DirectoryExists(url.GetFilename());
    else
    {
      kodi::Log(ADDON_LOG_ERROR, "SFTPFile: Failed to create session to check exists");
      return false;
    }
  }

  bool GetDirectory(const kodi::addon::VFSUrl& url,
                    std::vector<kodi::vfs::CDirEntry>& items,
                    CVFSCallbacks callbacks) override
  {
    CSFTPSessionPtr session = CSFTPSessionManager::Get().CreateSession(url);
    std::stringstream str;
    str << url.GetProtocol() << "://" << url.GetUsername() << ":" << url.GetPassword() << "@"
        << url.GetHostname() << ":" << (url.GetPort() ? url.GetPort() : 22) << "/";

    return session->GetDirectory(str.str(), url.GetFilename(), items);
  }

  bool Delete(const kodi::addon::VFSUrl& url) override
  {
    CSFTPSessionPtr session = CSFTPSessionManager::Get().CreateSession(url);
    if (session)
      return session->DeleteFile(url.GetFilename());
    else
    {
      kodi::Log(ADDON_LOG_ERROR, "SFTPFile: Failed to create session to delete file '%s'",
                url.GetFilename().c_str());
      return false;
    }
  }

  bool RemoveDirectory(const kodi::addon::VFSUrl& url) override
  {
    CSFTPSessionPtr session = CSFTPSessionManager::Get().CreateSession(url);
    if (session)
      return session->DeleteDirectory(url.GetFilename());
    else
    {
      kodi::Log(ADDON_LOG_ERROR, "SFTPFile: Failed to create session to delete folder '%s'",
                url.GetFilename().c_str());
      return false;
    }
  }

  bool CreateDirectory(const kodi::addon::VFSUrl& url) override
  {
    CSFTPSessionPtr session = CSFTPSessionManager::Get().CreateSession(url);
    if (session)
      return session->MakeDirectory(url.GetFilename());
    else
    {
      kodi::Log(ADDON_LOG_ERROR, "SFTPFile: Failed to create session to create folder '%s'",
                url.GetFilename().c_str());
      return false;
    }
  }

  bool Rename(const kodi::addon::VFSUrl& url_from, const kodi::addon::VFSUrl& url_to) override
  {
    CSFTPSessionPtr session = CSFTPSessionManager::Get().CreateSession(url_from);
    if (session)
      return session->RenameFile(url_from.GetFilename(), url_to.GetFilename());
    else
    {
      kodi::Log(ADDON_LOG_ERROR, "SFTPFile: Failed to create session to rename file '%s'",
                url_from.GetFilename().c_str());
      return false;
    }
  }

  bool ContainsFiles(const kodi::addon::VFSUrl& url,
                    std::vector<kodi::vfs::CDirEntry>& items,
                    std::string &rootPath) override
  {
    return DirectoryExists(url) && !items.empty();
  }

  kodi::addon::VFSFileHandle OpenForWrite(const kodi::addon::VFSUrl& url, bool overWrite) override
  {
    if (overWrite)
      return OpenInternal(url, O_RDWR | O_CREAT | O_TRUNC);
    else
      return OpenInternal(url, O_RDWR | O_CREAT);
  }

  int Truncate(kodi::addon::VFSFileHandle context, int64_t size) override
  {
    kodi::Log(ADDON_LOG_ERROR, "SFTPFile: Truncate is not implemented");
    return -1;
  }

  bool IoControlGetCacheStatus (kodi::addon::VFSFileHandle context, kodi::vfs::CacheStatus &status) override { return false; }

  bool IoControlSetCacheRate (kodi::addon::VFSFileHandle context, uint32_t rate) override { return false; }

  bool IoControlSetRetry (kodi::addon::VFSFileHandle context, bool retry) override { return false; }

private:
  //! Makes sure ctx->sftp_handle belongs to the current connection, reopening the
  //! file at the saved position after a reconnect.
  bool EnsureHandle(SFTPContext* ctx)
  {
    if (ctx->sftp_handle && ctx->generation == ctx->session->Generation())
      return true;

    // The old handle belonged to a freed SFTP session; it can't be closed safely.
    ctx->sftp_handle = nullptr;

    // Never truncate when reopening a file we were already writing.
    mode_t mode = ctx->mode & ~O_TRUNC;
    unsigned int generation = 0;
    sftp_file handle = ctx->session->CreateFileHande(ctx->file, mode, &generation);
    if (!handle)
      return false;

    if (ctx->position > 0 &&
        ctx->session->Seek(handle, static_cast<uint64_t>(ctx->position)) != 0)
    {
      ctx->session->CloseFileHandle(handle, generation);
      return false;
    }

    ctx->sftp_handle = handle;
    ctx->generation = generation;
    kodi::Log(ADDON_LOG_INFO, "SFTPFile: Reopened '%s' at %lld after reconnect",
              ctx->file.c_str(), static_cast<long long>(ctx->position));
    return true;
  }

  kodi::addon::VFSFileHandle OpenInternal(const kodi::addon::VFSUrl& url, mode_t mode)
  {
    SFTPContext* result = new SFTPContext;

    result->session = CSFTPSessionManager::Get().CreateSession(url);

    if (result->session)
    {
      result->file = url.GetFilename().c_str();
      result->mode = mode;
      result->sftp_handle =
          result->session->CreateFileHande(result->file, mode, &result->generation);
      if (result->sftp_handle)
        return result;
    }
    else
      kodi::Log(ADDON_LOG_ERROR, "SFTPFile: Failed to allocate session");

    delete result;
    return nullptr;
  }
};

class ATTR_DLL_LOCAL CMyAddon : public kodi::addon::CAddonBase
{
public:
  CMyAddon() { ssh_init(); }

  ~CMyAddon() override { ssh_finalize(); }

  ADDON_STATUS CreateInstance(const kodi::addon::IInstanceInfo& instance,
                              KODI_ADDON_INSTANCE_HDL& hdl) override
  {
    hdl = new CSFTPFile(instance);
    return ADDON_STATUS_OK;
  }
};

ADDONCREATOR(CMyAddon);
