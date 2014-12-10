/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "mozilla/dom/cache/QuotaClient.h"

#include "mozilla/DebugOnly.h"
#include "mozilla/dom/quota/QuotaManager.h"
#include "mozilla/dom/quota/UsageInfo.h"
#include "nsIFile.h"
#include "nsISimpleEnumerator.h"

namespace {

using mozilla::dom::quota::UsageInfo;

static nsresult
GetBodyUsage(nsIFile* aDir, UsageInfo* aUsageInfo)
{
  nsCOMPtr<nsISimpleEnumerator> entries;
  nsresult rv = aDir->GetDirectoryEntries(getter_AddRefs(entries));
  if (NS_WARN_IF(NS_FAILED(rv))) { return rv; }

  bool hasMore;
  while (NS_SUCCEEDED(rv = entries->HasMoreElements(&hasMore)) && hasMore &&
         !aUsageInfo->Canceled()) {
    nsCOMPtr<nsISupports> entry;
    rv = entries->GetNext(getter_AddRefs(entry));
    if (NS_WARN_IF(NS_FAILED(rv))) { return rv; }

    nsCOMPtr<nsIFile> file = do_QueryInterface(entry);
    if (!file) { return NS_NOINTERFACE; }

    bool isDir;
    rv = file->IsDirectory(&isDir);
    if (NS_WARN_IF(NS_FAILED(rv))) { return rv; }

    if (isDir) {
      rv = GetBodyUsage(file, aUsageInfo);
      if (NS_WARN_IF(NS_FAILED(rv))) { return rv; }
      continue;
    }

    int64_t fileSize;
    rv = file->GetFileSize(&fileSize);
    if (NS_WARN_IF(NS_FAILED(rv))) { return rv; }
    MOZ_ASSERT(fileSize >= 0);

    aUsageInfo->AppendToFileUsage(fileSize);
  }

  return NS_OK;
}

} // anonymous namespace;

namespace mozilla {
namespace dom {
namespace cache {

using mozilla::dom::quota::Client;
using mozilla::dom::quota::PersistenceType;
using mozilla::dom::quota::QuotaManager;
using mozilla::dom::quota::UsageInfo;

Client*
QuotaClient::Create()
{
  return new QuotaClient();
}

Client::Type
QuotaClient::GetType()
{
  return DOMCACHE;
}

nsresult
QuotaClient::InitOrigin(PersistenceType aPersistenceType,
                        const nsACString& aGroup, const nsACString& aOrigin,
                        UsageInfo* aUsageInfo)
{
  return NS_OK;
}

nsresult
QuotaClient::GetUsageForOrigin(PersistenceType aPersistenceType,
                               const nsACString& aGroup,
                               const nsACString& aOrigin, UsageInfo* aUsageInfo)
{
  QuotaManager* qm = QuotaManager::Get();
  MOZ_ASSERT(qm);

  nsCOMPtr<nsIFile> dir;
  nsresult rv = qm->GetDirectoryForOrigin(aPersistenceType, aOrigin,
                                          getter_AddRefs(dir));
  if (NS_WARN_IF(NS_FAILED(rv))) { return rv; }

  rv = dir->Append(NS_LITERAL_STRING(DOMCACHE_DIRECTORY_NAME));
  if (NS_WARN_IF(NS_FAILED(rv))) { return rv; }

  DebugOnly<bool> exists;
  MOZ_ASSERT(NS_SUCCEEDED(dir->Exists(&exists)) && exists);

  nsCOMPtr<nsISimpleEnumerator> entries;
  rv = dir->GetDirectoryEntries(getter_AddRefs(entries));
  if (NS_WARN_IF(NS_FAILED(rv))) { return rv; }

  bool hasMore;
  while (NS_SUCCEEDED(rv = entries->HasMoreElements(&hasMore)) && hasMore &&
         !aUsageInfo->Canceled()) {
    nsCOMPtr<nsISupports> entry;
    rv = entries->GetNext(getter_AddRefs(entry));
    if (NS_WARN_IF(NS_FAILED(rv))) { return rv; }

    nsCOMPtr<nsIFile> file = do_QueryInterface(entry);
    if (!file) { return NS_NOINTERFACE; }

    nsAutoString leafName;
    rv = file->GetLeafName(leafName);
    if (NS_WARN_IF(NS_FAILED(rv))) { return rv; }

    bool isDir;
    rv = file->IsDirectory(&isDir);
    if (NS_WARN_IF(NS_FAILED(rv))) { return rv; }

    if (isDir) {
      if (leafName.EqualsLiteral("morgue")) {
        rv = GetBodyUsage(file, aUsageInfo);
      } else {
        NS_WARNING("Unknown Cache directory found!");
      }

      continue;
    }

    // Ignore transient sqlite files
    if (leafName.EqualsLiteral("db.sqlite-journal") ||
        leafName.EqualsLiteral("db.sqlite-shm") ||
        leafName.Find(NS_LITERAL_CSTRING("db.sqlite-mj"), false, 0, 0) == 0) {
      continue;
    }

    if (leafName.EqualsLiteral("db.sqlite") ||
        leafName.EqualsLiteral("db.sqlite-wal")) {
      int64_t fileSize;
      rv = file->GetFileSize(&fileSize);
      if (NS_WARN_IF(NS_FAILED(rv))) { return rv; }
      MOZ_ASSERT(fileSize >= 0);

      aUsageInfo->AppendToDatabaseUsage(fileSize);
      continue;
    }

    NS_WARNING("Unknown Cache file found!");
  }

  return NS_OK;
}

void
QuotaClient::OnOriginClearCompleted(PersistenceType aPersistenceType,
                                    const nsACString& aOrigin)
{
}

void
QuotaClient::ReleaseIOThreadObjects()
{
  // nothing to do
}

bool
QuotaClient::IsFileServiceUtilized()
{
  return false;
}

bool
QuotaClient::IsTransactionServiceActivated()
{
  return false;
}

void
QuotaClient::WaitForStoragesToComplete(nsTArray<nsIOfflineStorage*>& aStorages,
                                       nsIRunnable* aCallback)
{
  // nothing to do
}

void
QuotaClient::ShutdownTransactionService()
{
  // nothing to do
}

QuotaClient::QuotaClient()
{
}

QuotaClient::~QuotaClient()
{
}


} // namespace cache
} // namespace dom
} // namespace mozilla
