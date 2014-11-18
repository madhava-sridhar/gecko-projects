/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "mozilla/dom/cache/DBAction.h"

#include "mozilla/dom/quota/PersistenceType.h"
#include "mozIStorageConnection.h"
#include "mozIStorageService.h"
#include "mozStorageCID.h"
#include "nsIFile.h"
#include "nsIURI.h"
#include "nsNetUtil.h"

namespace mozilla {
namespace dom {
namespace cache {

using mozilla::dom::quota::PERSISTENCE_TYPE_PERSISTENT;
using mozilla::dom::quota::PersistenceType;

DBAction::DBAction(Mode aMode, const CacheInitData& aInitData)
  : mMode(aMode)
  , mInitData(aInitData)
{
}

void
DBAction::RunOnTarget(Resolver* aResolver, nsIFile* aQuotaDir)
{
  MOZ_ASSERT(aResolver);
  MOZ_ASSERT(aQuotaDir);

  nsresult rv = aQuotaDir->Append(NS_LITERAL_STRING("cache"));
  if (NS_WARN_IF(NS_FAILED(rv))) {
    aResolver->Resolve(rv);
    return;
  }

  nsCOMPtr<mozIStorageConnection> conn;
  rv = OpenConnection(aQuotaDir, getter_AddRefs(conn));
  if (NS_WARN_IF(NS_FAILED(rv))) {
    aResolver->Resolve(rv);
    return;
  }
  MOZ_ASSERT(conn);

  RunWithDBOnTarget(aResolver, aQuotaDir, conn);
}

nsresult
DBAction::OpenConnection(nsIFile* aDBDir, mozIStorageConnection** aConnOut)
{
  MOZ_ASSERT(aDBDir);
  MOZ_ASSERT(aConnOut);

  bool exists;
  nsresult rv = aDBDir->Exists(&exists);
  if (NS_WARN_IF(NS_FAILED(rv))) { return rv; }

  if (!exists) {
    if (NS_WARN_IF(mMode != Create)) {  return NS_ERROR_FILE_NOT_FOUND; }
    rv = aDBDir->Create(nsIFile::DIRECTORY_TYPE, 0755);
    if (NS_WARN_IF(NS_FAILED(rv))) { return rv; }
  }

  nsCOMPtr<nsIFile> dbFile;
  rv = aDBDir->Clone(getter_AddRefs(dbFile));
  if (NS_WARN_IF(NS_FAILED(rv))) { return rv; }

  rv = dbFile->Append(NS_LITERAL_STRING("db.sqlite"));
  if (NS_WARN_IF(NS_FAILED(rv))) { return rv; }

  rv = dbFile->Exists(&exists);
  if (NS_WARN_IF(NS_FAILED(rv))) { return rv; }

  nsCOMPtr<nsIFile> dbTmpDir;
  rv = aDBDir->Clone(getter_AddRefs(dbTmpDir));
  if (NS_WARN_IF(NS_FAILED(rv))) { return rv; }

  rv = dbTmpDir->Append(NS_LITERAL_STRING("db"));
  if (NS_WARN_IF(NS_FAILED(rv))) { return rv; }

  // XXX: Jonas tells me nsIFileURL usage off-main-thread is dangerous,
  //      but this is what IDB does to access mozIStorageConnection so
  //      it seems at least this corner case mostly works.
  nsCOMPtr<nsIURI> uri;
  rv = NS_NewFileURI(getter_AddRefs(uri), dbFile);
  if (NS_WARN_IF(NS_FAILED(rv))) { return rv; }

  nsCOMPtr<nsIFileURL> dbFileUrl = do_QueryInterface(uri);
  if (NS_WARN_IF(!dbFileUrl)) { return NS_ERROR_UNEXPECTED; }

  nsAutoCString type;
  PersistenceTypeToText(PERSISTENCE_TYPE_PERSISTENT, type);

  rv = dbFileUrl->SetQuery(
    NS_LITERAL_CSTRING("persistenceType=") + type +
    NS_LITERAL_CSTRING("&group=") + mInitData.quotaGroup() +
    NS_LITERAL_CSTRING("&origin=") + mInitData.origin());
  if (NS_WARN_IF(NS_FAILED(rv))) { return rv; }

  nsCOMPtr<mozIStorageService> ss =
    do_GetService(MOZ_STORAGE_SERVICE_CONTRACTID);
  if (NS_WARN_IF(!ss)) { return NS_ERROR_UNEXPECTED; }

  rv = ss->OpenDatabaseWithFileURL(dbFileUrl, aConnOut);
  if (rv == NS_ERROR_FILE_CORRUPTED) {
    dbFile->Remove(false);
    if (NS_WARN_IF(NS_FAILED(rv))) { return rv; }

    rv = dbTmpDir->Exists(&exists);
    if (NS_WARN_IF(NS_FAILED(rv))) { return rv; }

    if (exists) {
      bool isDir;
      rv = dbTmpDir->IsDirectory(&isDir);
      if (NS_WARN_IF(NS_FAILED(rv))) { return rv; }
      if (NS_WARN_IF(!isDir)) { return NS_ERROR_UNEXPECTED; }
      rv = dbTmpDir->Remove(true);
      if (NS_WARN_IF(NS_FAILED(rv))) { return rv; }
    }

    rv = ss->OpenDatabaseWithFileURL(dbFileUrl, aConnOut);
  }
  if (NS_WARN_IF(NS_FAILED(rv))) { return rv; }
  MOZ_ASSERT(*aConnOut);
  return rv;
}

SyncDBAction::SyncDBAction(Mode aMode, const CacheInitData& aInitData)
  : DBAction(aMode, aInitData)
{
}

void
SyncDBAction::RunWithDBOnTarget(Resolver* aResolver, nsIFile* aQuotaDir,
                                mozIStorageConnection* aConn)
{
  MOZ_ASSERT(aResolver);
  MOZ_ASSERT(aQuotaDir);
  MOZ_ASSERT(aConn);

  nsresult rv = RunSyncWithDBOnTarget(aQuotaDir, aConn);
  aResolver->Resolve(rv);
}

} // namespace cache
} // namespace dom
} // namespace mozilla
