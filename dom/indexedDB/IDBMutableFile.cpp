/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "IDBMutableFile.h"

#include "FileInfo.h"
#include "IDBDatabase.h"
#include "IDBFactory.h"
#include "IndexedDatabaseManager.h"
#include "MainThreadUtils.h"
#include "mozilla/Assertions.h"
#include "mozilla/dom/File.h"
#include "mozilla/dom/IDBMutableFileBinding.h"
#include "mozilla/dom/indexedDB/PBackgroundIDBSharedTypes.h"
#include "mozilla/dom/quota/FileStreams.h"
#include "mozilla/dom/quota/QuotaManager.h"
#include "mozilla/ipc/BackgroundUtils.h"
#include "mozilla/ipc/PBackgroundSharedTypes.h"
#include "nsIPrincipal.h"

using namespace mozilla;
using namespace mozilla::dom;
using namespace mozilla::dom::indexedDB;
using namespace mozilla::dom::quota;
using namespace mozilla::ipc;

namespace {

already_AddRefed<nsIFile>
GetFileFor(FileInfo* aFileInfo)

{
  MOZ_ASSERT(IndexedDatabaseManager::IsMainProcess());
  MOZ_ASSERT(NS_IsMainThread());
  MOZ_ASSERT(aFileInfo);

  FileManager* fileManager = aFileInfo->Manager();
  MOZ_ASSERT(fileManager);

  nsCOMPtr<nsIFile> directory = fileManager->GetDirectory();
  if (NS_WARN_IF(!directory)) {
    return nullptr;
  }

  nsCOMPtr<nsIFile> file =
    fileManager->GetFileForId(directory, aFileInfo->Id());
  if (NS_WARN_IF(!file)) {
    return nullptr;
  }

  return file.forget();
}

} // anonymous namespace

IDBMutableFile::IDBMutableFile(IDBDatabase* aDatabase,
                               const nsAString& aName,
                               const nsAString& aType,
                               already_AddRefed<FileInfo> aFileInfo,
                               const nsACString& aGroup,
                               const nsACString& aOrigin,
                               const nsACString& aStorageId,
                               PersistenceType aPersistenceType,
                               already_AddRefed<nsIFile> aFile)
  : MutableFile(aDatabase)
  , mDatabase(aDatabase)
  , mFileInfo(aFileInfo)
  , mGroup(aGroup)
  , mOrigin(aOrigin)
  , mPersistenceType(aPersistenceType)
{
  MOZ_ASSERT(IndexedDatabaseManager::IsMainProcess());
  MOZ_ASSERT(NS_IsMainThread());
  MOZ_ASSERT(mDatabase);
  MOZ_ASSERT(mFileInfo);

  mName = aName;
  mType = aType;
  mFile = aFile;
  mStorageId = aStorageId;
  mFileName.AppendInt(mFileInfo->Id());

  MOZ_ASSERT(mFile);
}

IDBMutableFile::~IDBMutableFile()
{
  MOZ_ASSERT(IndexedDatabaseManager::IsMainProcess());
  MOZ_ASSERT(NS_IsMainThread());
}

// static
already_AddRefed<IDBMutableFile>
IDBMutableFile::Create(IDBDatabase* aDatabase,
                       const nsAString& aName,
                       const nsAString& aType,
                       already_AddRefed<FileInfo> aFileInfo)
{
  MOZ_ASSERT(IndexedDatabaseManager::IsMainProcess());
  MOZ_ASSERT(NS_IsMainThread());

  nsRefPtr<FileInfo> fileInfo(aFileInfo);
  MOZ_ASSERT(fileInfo);

  PrincipalInfo* principalInfo = aDatabase->Factory()->GetPrincipalInfo();
  MOZ_ASSERT(principalInfo);

  nsCOMPtr<nsIPrincipal> principal = PrincipalInfoToPrincipal(*principalInfo);
  if (NS_WARN_IF(!principal)) {
    return nullptr;
  }

  nsCString group;
  nsCString origin;
  if (NS_WARN_IF(NS_FAILED(QuotaManager::GetInfoFromPrincipal(principal,
                                                              &group,
                                                              &origin,
                                                              nullptr,
                                                              nullptr)))) {
    return nullptr;
  }

  const DatabaseSpec* spec = aDatabase->Spec();
  MOZ_ASSERT(spec);

  PersistenceType persistenceType = spec->metadata().persistenceType();

  nsCString storageId;
  QuotaManager::GetStorageId(persistenceType,
                             origin,
                             Client::IDB,
                             aName,
                             storageId);

  nsCOMPtr<nsIFile> file = GetFileFor(fileInfo);
  if (NS_WARN_IF(!file)) {
    return nullptr;
  }

  nsRefPtr<IDBMutableFile> newFile =
    new IDBMutableFile(aDatabase,
                       aName,
                       aType,
                       fileInfo.forget(),
                       group,
                       origin,
                       storageId,
                       persistenceType,
                       file.forget());

  return newFile.forget();
}

IDBDatabase*
IDBMutableFile::Database() const
{
  MOZ_ASSERT(NS_IsMainThread());

  return mDatabase;
}

NS_IMPL_CYCLE_COLLECTION_INHERITED(IDBMutableFile, MutableFile, mDatabase)

NS_INTERFACE_MAP_BEGIN_CYCLE_COLLECTION_INHERITED(IDBMutableFile)
NS_INTERFACE_MAP_END_INHERITING(MutableFile)

NS_IMPL_ADDREF_INHERITED(IDBMutableFile, MutableFile)
NS_IMPL_RELEASE_INHERITED(IDBMutableFile, MutableFile)

int64_t
IDBMutableFile::GetFileId()
{
  return mFileInfo->Id();
}

FileInfo*
IDBMutableFile::GetFileInfo()
{
  return mFileInfo;
}

bool
IDBMutableFile::IsShuttingDown()
{
  return QuotaManager::IsShuttingDown() || MutableFile::IsShuttingDown();
}

bool
IDBMutableFile::IsInvalid()
{
  return mDatabase->IsInvalidated();
}

nsIOfflineStorage*
IDBMutableFile::Storage()
{
  MOZ_CRASH("Don't call me!");
}

already_AddRefed<nsISupports>
IDBMutableFile::CreateStream(nsIFile* aFile, bool aReadOnly)
{
  MOZ_ASSERT(IndexedDatabaseManager::IsMainProcess());

  nsCOMPtr<nsISupports> result;

  if (aReadOnly) {
    nsRefPtr<FileInputStream> stream =
      FileInputStream::Create(mPersistenceType,
                              mGroup,
                              mOrigin,
                              aFile,
                              -1,
                              -1,
                              nsIFileInputStream::DEFER_OPEN);
    result = NS_ISUPPORTS_CAST(nsIFileInputStream*, stream);
  } else {
    nsRefPtr<FileStream> stream =
      FileStream::Create(mPersistenceType,
                         mGroup,
                         mOrigin,
                         aFile,
                         -1,
                         -1,
                         nsIFileStream::DEFER_OPEN);
    result = NS_ISUPPORTS_CAST(nsIFileStream*, stream);
  }

  if (NS_WARN_IF(!result)) {
    return nullptr;
  }

  return result.forget();
}

void
IDBMutableFile::SetThreadLocals()
{
  MOZ_ASSERT(IndexedDatabaseManager::IsMainProcess());
  MOZ_ASSERT(NS_IsMainThread());
  MOZ_ASSERT(mDatabase->GetOwner(), "Should have owner!");

  QuotaManager::SetCurrentWindow(mDatabase->GetOwner());
}

void
IDBMutableFile::UnsetThreadLocals()
{
  MOZ_ASSERT(IndexedDatabaseManager::IsMainProcess());
  MOZ_ASSERT(NS_IsMainThread());

  QuotaManager::SetCurrentWindow(nullptr);
}

already_AddRefed<nsIDOMFile>
IDBMutableFile::CreateFileObject(FileHandle* aFileHandle, uint32_t aFileSize)
{
  MOZ_ASSERT(IndexedDatabaseManager::IsMainProcess());
  MOZ_ASSERT(NS_IsMainThread());

  nsRefPtr<FileImpl> impl =
    new FileImpl(mName, mType, aFileSize, mFile, aFileHandle, mFileInfo);

  nsCOMPtr<nsIDOMFile> file = new DOMFile(impl);

  return file.forget();
}

JSObject*
IDBMutableFile::WrapObject(JSContext* aCx)
{
  MOZ_ASSERT(IndexedDatabaseManager::IsMainProcess());
  MOZ_ASSERT(NS_IsMainThread());

  return IDBMutableFileBinding::Wrap(aCx, this);
}
