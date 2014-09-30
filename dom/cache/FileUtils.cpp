/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "mozilla/dom/cache/FileUtils.h"
#include "mozilla/dom/quota/FileStreams.h"
#include "mozilla/unused.h"
#include "nsIFile.h"
#include "nsIUUIDGenerator.h"
#include "nsServiceManagerUtils.h"
#include "nsString.h"
#include "nsThreadUtils.h"

namespace mozilla {
namespace dom {
namespace cache {

using mozilla::dom::quota::FileInputStream;
using mozilla::dom::quota::FileOutputStream;
using mozilla::dom::quota::PERSISTENCE_TYPE_PERSISTENT;
using mozilla::unused;

// static
nsresult
FileUtils::BodyCreateDir(nsIFile* aBaseDir)
{
  MOZ_ASSERT(aBaseDir);

  nsCOMPtr<nsIFile> aBodyDir;
  nsresult rv = aBaseDir->Clone(getter_AddRefs(aBodyDir));
  if (NS_WARN_IF(NS_FAILED(rv))) { return rv; }

  rv = aBodyDir->Append(NS_LITERAL_STRING("morgue"));
  if (NS_WARN_IF(NS_FAILED(rv))) { return rv; }

  bool exists;
  rv = aBodyDir->Exists(&exists);
  if (NS_WARN_IF(NS_FAILED(rv))) { return rv; }

  if (!exists) {
    rv = aBodyDir->Create(nsIFile::DIRECTORY_TYPE, 0755);
    if (NS_WARN_IF(NS_FAILED(rv))) { return rv; }
  } else {
    bool isDir;
    rv = aBodyDir->IsDirectory(&isDir);
    if (NS_WARN_IF(NS_FAILED(rv))) { return rv; }
    if (NS_WARN_IF(!isDir)) { return NS_ERROR_FILE_NOT_DIRECTORY; }
  }

  return rv;
}

// static
nsresult
FileUtils::BodyGetCacheDir(nsIFile* aBaseDir, CacheId aCacheId,
                           nsIFile** aCacheDirOut)
{
  MOZ_ASSERT(aBaseDir);
  MOZ_ASSERT(aCacheDirOut);

  nsresult rv = aBaseDir->Clone(aCacheDirOut);
  if (NS_WARN_IF(NS_FAILED(rv))) { return rv; }

  rv = (*aCacheDirOut)->Append(NS_LITERAL_STRING("morgue"));
  if (NS_WARN_IF(NS_FAILED(rv))) { return rv; }

  bool exists;
  rv = (*aCacheDirOut)->Exists(&exists);
  if (NS_WARN_IF(NS_FAILED(rv))) { return rv; }
  if (!exists) { return NS_ERROR_FILE_NOT_FOUND; }

  bool isDir;
  rv = (*aCacheDirOut)->IsDirectory(&isDir);
  if (NS_WARN_IF(NS_FAILED(rv))) { return rv; }
  if (NS_WARN_IF(!isDir)) { return NS_ERROR_FILE_NOT_DIRECTORY; }

  nsAutoString cacheDirName;
  cacheDirName.AppendInt(aCacheId);
  rv = (*aCacheDirOut)->Append(cacheDirName);
  if (NS_WARN_IF(NS_FAILED(rv))) { return rv; }

  rv = (*aCacheDirOut)->Exists(&exists);
  if (NS_WARN_IF(NS_FAILED(rv))) { return rv; }

  if (!exists) {
    rv = (*aCacheDirOut)->Create(nsIFile::DIRECTORY_TYPE, 0755);
    if (NS_WARN_IF(NS_FAILED(rv))) { return rv; }
  } else {
    rv = (*aCacheDirOut)->IsDirectory(&isDir);
    if (NS_WARN_IF(NS_FAILED(rv))) { return rv; }
    if (NS_WARN_IF(!isDir)) { return NS_ERROR_FILE_NOT_DIRECTORY; }
  }

  return rv;
}

// static
nsresult
FileUtils::BodyDeleteCacheDir(nsIFile* aBaseDir, CacheId aCacheId)
{
  MOZ_ASSERT(aBaseDir);

  nsCOMPtr<nsIFile> cacheDir;
  nsresult rv = aBaseDir->Clone(getter_AddRefs(cacheDir));
  if (NS_WARN_IF(NS_FAILED(rv))) { return rv; }

  rv = cacheDir->Append(NS_LITERAL_STRING("morgue"));
  if (NS_WARN_IF(NS_FAILED(rv))) { return rv; }

  bool exists;
  rv = cacheDir->Exists(&exists);
  if (NS_WARN_IF(NS_FAILED(rv))) { return rv; }
  if (NS_WARN_IF(!exists)) { return NS_ERROR_FILE_NOT_DIRECTORY; }

  nsAutoString cacheDirName;
  cacheDirName.AppendInt(aCacheId);
  rv = cacheDir->Append(cacheDirName);
  if (NS_WARN_IF(NS_FAILED(rv))) { return rv; }

  rv = cacheDir->Exists(&exists);
  if (NS_WARN_IF(NS_FAILED(rv))) { return rv; }

  // Already in desired state where cache dir does not exist.
  if (!exists) {
    return rv;
  }

  bool isDir;
  rv = cacheDir->IsDirectory(&isDir);
  if (NS_WARN_IF(NS_FAILED(rv))) { return rv; }
  if (NS_WARN_IF(!isDir)) { return NS_ERROR_FILE_NOT_DIRECTORY; }

  rv = cacheDir->Remove(true /* recursive */);
  if (NS_WARN_IF(NS_FAILED(rv))) { return rv; }

  return rv;
}

// static
nsresult
FileUtils::BodyIdToFile(nsIFile* aBaseDir, CacheId aCacheId, const nsID& aId,
                        BodyFileType aType, nsIFile** aBodyFileOut)
{
  MOZ_ASSERT(aBaseDir);
  MOZ_ASSERT(aBodyFileOut);

  nsresult rv = BodyGetCacheDir(aBaseDir, aCacheId, aBodyFileOut);
  if (NS_WARN_IF(NS_FAILED(rv))) { return rv; }

  bool exists;
  rv = (*aBodyFileOut)->Exists(&exists);
  if (NS_WARN_IF(NS_FAILED(rv))) { return rv; }
  if (!exists) { return NS_ERROR_FILE_NOT_FOUND; }

  bool isDir;
  rv = (*aBodyFileOut)->IsDirectory(&isDir);
  if (NS_WARN_IF(NS_FAILED(rv))) { return rv; }
  if (NS_WARN_IF(!isDir)) { return NS_ERROR_FILE_NOT_DIRECTORY; }

  char idString[NSID_LENGTH];
  aId.ToProvidedString(idString);

  NS_ConvertUTF8toUTF16 fileName(idString);

  if (aType == BODY_FILE_FINAL) {
    fileName.Append(NS_LITERAL_STRING(".final"));
  } else {
    fileName.Append(NS_LITERAL_STRING(".tmp"));
  }

  rv = (*aBodyFileOut)->Append(fileName);
  if (NS_WARN_IF(NS_FAILED(rv))) { return rv; }

  return rv;
}

// static
nsresult
FileUtils::BodyStartWriteStream(const nsACString& aOrigin,
                                const nsACString& aBaseDomain,
                                nsIFile* aBaseDir, CacheId aCacheId,
                                nsIInputStream* aSource, void* aClosure,
                                nsAsyncCopyCallbackFun aCallback,
                                nsID* aIdOut, nsISupports** aCopyContextOut)
{
  MOZ_ASSERT(aBaseDir);
  MOZ_ASSERT(aSource);
  MOZ_ASSERT(aClosure);
  MOZ_ASSERT(aCallback);
  MOZ_ASSERT(aIdOut);
  MOZ_ASSERT(aCopyContextOut);

  nsresult rv;
  nsCOMPtr<nsIUUIDGenerator> idGen =
    do_GetService("@mozilla.org/uuid-generator;1", &rv);
  if (NS_WARN_IF(NS_FAILED(rv))) { return rv; }

  rv = idGen->GenerateUUIDInPlace(aIdOut);
  if (NS_WARN_IF(NS_FAILED(rv))) { return rv; }

  nsCOMPtr<nsIFile> finalFile;
  rv = BodyIdToFile(aBaseDir, aCacheId, *aIdOut, BODY_FILE_FINAL,
                    getter_AddRefs(finalFile));
  if (NS_WARN_IF(NS_FAILED(rv))) { return rv; }

  bool exists;
  rv = finalFile->Exists(&exists);
  if (NS_WARN_IF(NS_FAILED(rv))) { return rv; }
  if (NS_WARN_IF(exists)) { return NS_ERROR_FILE_ALREADY_EXISTS; }

  nsCOMPtr<nsIFile> tmpFile;
  rv = BodyIdToFile(aBaseDir, aCacheId, *aIdOut, BODY_FILE_TMP,
                    getter_AddRefs(tmpFile));
  if (NS_WARN_IF(NS_FAILED(rv))) { return rv; }

  rv = tmpFile->Exists(&exists);
  if (NS_WARN_IF(NS_FAILED(rv))) { return rv; }
  if (NS_WARN_IF(exists)) { return NS_ERROR_FILE_ALREADY_EXISTS; }

  nsCOMPtr<nsIOutputStream> fileStream =
    FileOutputStream::Create(PERSISTENCE_TYPE_PERSISTENT,
                             aBaseDomain, aOrigin, tmpFile);
  if (NS_WARN_IF(!fileStream)) { return NS_ERROR_UNEXPECTED; }

  rv = NS_AsyncCopy(aSource, fileStream, NS_GetCurrentThread(),
                    NS_ASYNCCOPY_VIA_READSEGMENTS,
                    4096, // chunk size
                    aCallback, aClosure,
                    true, true, // close streams
                    aCopyContextOut);
  if (NS_WARN_IF(NS_FAILED(rv))) {
    fileStream->Close();
    return rv;
  }

  return rv;
}

// static
void
FileUtils::BodyCancelWrite(nsIFile* aBaseDir, CacheId aCacheId, const nsID& aId,
                           nsISupports* aCopyContext)
{
  MOZ_ASSERT(aBaseDir);
  MOZ_ASSERT(aCopyContext);

  nsresult rv = NS_CancelAsyncCopy(aCopyContext, NS_ERROR_ABORT);
  unused << NS_WARN_IF(NS_FAILED(rv));

  nsCOMPtr<nsIFile> tmpFile;
  rv = BodyIdToFile(aBaseDir, aCacheId, aId, BODY_FILE_TMP,
                    getter_AddRefs(tmpFile));
  if (NS_WARN_IF(NS_FAILED(rv))) { return; }

  rv = tmpFile->Remove(false /* recursive */);
  unused << NS_WARN_IF(NS_FAILED(rv));
}

// static
nsresult
FileUtils::BodyFinalizeWrite(nsIFile* aBaseDir, CacheId aCacheId, const nsID& aId)
{
  MOZ_ASSERT(aBaseDir);

  nsCOMPtr<nsIFile> tmpFile;
  nsresult rv = BodyIdToFile(aBaseDir, aCacheId, aId, BODY_FILE_TMP,
                             getter_AddRefs(tmpFile));
  if (NS_WARN_IF(NS_FAILED(rv))) { return rv; }

  bool exists;
  rv = tmpFile->Exists(&exists);
  if (NS_WARN_IF(NS_FAILED(rv))) { return rv; }
  if (NS_WARN_IF(!exists)) { return NS_ERROR_FILE_NOT_FOUND; }

  nsCOMPtr<nsIFile> finalFile;
  rv = BodyIdToFile(aBaseDir, aCacheId, aId, BODY_FILE_FINAL,
                    getter_AddRefs(finalFile));
  if (NS_WARN_IF(NS_FAILED(rv))) { return rv; }

  rv = finalFile->Exists(&exists);
  if (NS_WARN_IF(NS_FAILED(rv))) { return rv; }
  if (NS_WARN_IF(exists)) { return NS_ERROR_FILE_ALREADY_EXISTS; }

  nsCOMPtr<nsIFile> finalDir;
  rv = finalFile->GetParent(getter_AddRefs(finalDir));
  if (NS_WARN_IF(NS_FAILED(rv))) { return rv; }

  nsAutoString finalFileName;
  rv = finalFile->GetLeafName(finalFileName);
  if (NS_WARN_IF(NS_FAILED(rv))) { return rv; }

  rv = tmpFile->RenameTo(finalDir, finalFileName);
  if (NS_WARN_IF(NS_FAILED(rv))) { return rv; }

  return rv;
}

// static
nsresult
FileUtils::BodyOpen(const nsACString& aOrigin, const nsACString& aBaseDomain,
                    nsIFile* aBaseDir, CacheId aCacheId, const nsID& aId,
                    nsIInputStream** aStreamOut)
{
  MOZ_ASSERT(aBaseDir);
  MOZ_ASSERT(aStreamOut);

  nsCOMPtr<nsIFile> finalFile;
  nsresult rv = BodyIdToFile(aBaseDir, aCacheId, aId, BODY_FILE_FINAL,
                    getter_AddRefs(finalFile));
  if (NS_WARN_IF(NS_FAILED(rv))) { return rv; }

  bool exists;
  rv = finalFile->Exists(&exists);
  if (NS_WARN_IF(NS_FAILED(rv))) { return rv; }
  if (NS_WARN_IF(!exists)) { return NS_ERROR_FILE_NOT_FOUND; }

  nsCOMPtr<nsIInputStream> fileStream =
    FileInputStream::Create(PERSISTENCE_TYPE_PERSISTENT, aBaseDomain, aOrigin,
                            finalFile);
  if (NS_WARN_IF(!fileStream)) { return NS_ERROR_UNEXPECTED; }

  fileStream.forget(aStreamOut);

  return rv;
}

// static
nsresult
FileUtils::BodyStartReadStream(const nsACString& aOrigin,
                               const nsACString& aBaseDomain,
                               nsIFile* aBaseDir, CacheId aCacheId,
                               const nsID& aId, nsIOutputStream* aDest,
                               void* aClosure,
                               nsAsyncCopyCallbackFun aCallback,
                               nsISupports** aCopyContextOut)
{
  MOZ_ASSERT(aBaseDir);
  MOZ_ASSERT(aDest);
  MOZ_ASSERT(aClosure);
  MOZ_ASSERT(aCallback);
  MOZ_ASSERT(aCopyContextOut);

  nsCOMPtr<nsIFile> finalFile;
  nsresult rv = BodyIdToFile(aBaseDir, aCacheId, aId, BODY_FILE_FINAL,
                    getter_AddRefs(finalFile));
  if (NS_WARN_IF(NS_FAILED(rv))) { return rv; }

  bool exists;
  rv = finalFile->Exists(&exists);
  if (NS_WARN_IF(NS_FAILED(rv))) { return rv; }
  if (NS_WARN_IF(!exists)) { return NS_ERROR_FILE_NOT_FOUND; }

  nsCOMPtr<nsIInputStream> fileStream =
    FileInputStream::Create(PERSISTENCE_TYPE_PERSISTENT, aBaseDomain, aOrigin,
                            finalFile);
  if (NS_WARN_IF(!fileStream)) { return NS_ERROR_UNEXPECTED; }

  rv = NS_AsyncCopy(fileStream, aDest, NS_GetCurrentThread(),
                    NS_ASYNCCOPY_VIA_READSEGMENTS,
                    4096, // chunk size
                    aCallback, aClosure,
                    true, true, // close streams
                    aCopyContextOut);
  if (NS_WARN_IF(NS_FAILED(rv))) {
    fileStream->Close();
    return rv;
  }

  return rv;
}

// static
void
FileUtils::BodyCancelRead(nsISupports* aCopyContext)
{
  MOZ_ASSERT(aCopyContext);
  nsresult rv = NS_CancelAsyncCopy(aCopyContext, NS_ERROR_ABORT);
  unused << NS_WARN_IF(NS_FAILED(rv));
}

// static
nsresult
FileUtils::BodyDeleteFiles(nsIFile* aBaseDir, CacheId aCacheId,
                           const nsTArray<nsID>& aIdList)
{
  nsresult rv = NS_OK;

  for (uint32_t i = 0; i < aIdList.Length(); ++i) {
    nsCOMPtr<nsIFile> finalFile;
    rv = BodyIdToFile(aBaseDir, aCacheId, aIdList[i], BODY_FILE_FINAL,
                      getter_AddRefs(finalFile));
    if (NS_WARN_IF(NS_FAILED(rv))) { return rv; }

    bool exists;
    rv = finalFile->Exists(&exists);
    if (NS_WARN_IF(NS_FAILED(rv))) { return rv; }

    if (NS_WARN_IF(!exists)) {
      continue;
    }

    rv = finalFile->Remove(false /* recursive */);
    if (NS_WARN_IF(NS_FAILED(rv))) { return rv; }
  }

  return rv;
}

} // namespace cache
} // namespace dom
} // namespace mozilla
