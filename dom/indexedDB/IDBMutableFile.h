/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_dom_indexeddb_idbmutablefile_h__
#define mozilla_dom_indexeddb_idbmutablefile_h__

#include "mozilla/Attributes.h"
#include "mozilla/dom/MutableFile.h"
#include "mozilla/dom/quota/PersistenceType.h"
#include "nsAutoPtr.h"
#include "nsCycleCollectionParticipant.h"

class nsIFile;

namespace mozilla {
namespace dom {
namespace indexedDB {

class FileInfo;
class IDBDatabase;

class IDBMutableFile MOZ_FINAL
  : public MutableFile
{
  typedef mozilla::dom::FileHandle FileHandle;
  typedef mozilla::dom::quota::PersistenceType PersistenceType;

  nsRefPtr<IDBDatabase> mDatabase;
  nsRefPtr<FileInfo> mFileInfo;

  const nsCString mGroup;
  const nsCString mOrigin;
  const PersistenceType mPersistenceType;

public:
  static already_AddRefed<IDBMutableFile>
  Create(IDBDatabase* aDatabase,
         const nsAString& aName,
         const nsAString& aType,
         already_AddRefed<FileInfo> aFileInfo);

  // WebIDL
  IDBDatabase*
  Database() const;

  // MutableFile
  NS_DECL_ISUPPORTS_INHERITED
  NS_DECL_CYCLE_COLLECTION_CLASS_INHERITED(IDBMutableFile, MutableFile)

  virtual int64_t
  GetFileId() MOZ_OVERRIDE;

  virtual FileInfo*
  GetFileInfo() MOZ_OVERRIDE;

  virtual bool
  IsShuttingDown() MOZ_OVERRIDE;

  virtual bool
  IsInvalid() MOZ_OVERRIDE;

  virtual nsIOfflineStorage*
  Storage() MOZ_OVERRIDE;

  virtual already_AddRefed<nsISupports>
  CreateStream(nsIFile* aFile, bool aReadOnly) MOZ_OVERRIDE;

  virtual void
  SetThreadLocals() MOZ_OVERRIDE;

  virtual void
  UnsetThreadLocals() MOZ_OVERRIDE;

  virtual already_AddRefed<nsIDOMFile>
  CreateFileObject(FileHandle* aFileHandle, uint32_t aFileSize) MOZ_OVERRIDE;

  // nsWrapperCache
  virtual JSObject*
  WrapObject(JSContext* aCx) MOZ_OVERRIDE;

private:
  IDBMutableFile(IDBDatabase* aDatabase,
                 const nsAString& aName,
                 const nsAString& aType,
                 already_AddRefed<FileInfo> aFileInfo,
                 const nsACString& aGroup,
                 const nsACString& aOrigin,
                 const nsACString& aStorageId,
                 PersistenceType aPersistenceType,
                 already_AddRefed<nsIFile> aFile);

  ~IDBMutableFile();
};

} // namespace indexedDB
} // namespace dom
} // namespace mozilla

#endif // mozilla_dom_indexeddb_idbmutablefile_h__
