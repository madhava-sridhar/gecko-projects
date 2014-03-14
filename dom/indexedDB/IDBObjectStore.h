/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_dom_indexeddb_idbobjectstore_h__
#define mozilla_dom_indexeddb_idbobjectstore_h__

#include "js/RootingAPI.h"
#include "mozilla/dom/IDBCursorBinding.h"
#include "mozilla/dom/IDBIndexBinding.h"
#include "nsAutoPtr.h"
#include "nsCycleCollectionParticipant.h"
#include "nsString.h"
#include "nsTArray.h"
#include "nsWrapperCache.h"

class JSAutoStructuredCloneBuffer;
class mozIStorageStatement;
template <typename> class nsCOMPtr;
class nsIDOMBlob;
class nsPIDOMWindow;

namespace mozilla {

class ErrorResult;

namespace dom {

class ContentParent;
class DOMStringList;
class PBlobChild;
class PBlobParent;
template <typename> class Sequence;

namespace indexedDB {

struct BlobOrFileData;
struct FileHandleData;
class FileManager;
class IDBCursor;
class IDBDatabase;
class IDBKeyRange;
class IDBRequest;
class IDBTransaction;
struct IndexInfo;
class IndexUpdateInfo;
class Key;
class KeyPath;
struct ObjectStoreInfo;
class ObjectStoreSpec;
class SerializedStructuredCloneReadInfo;
class SerializedStructuredCloneWriteInfo;
struct StructuredCloneFile;
struct StructuredCloneReadInfo;
struct StructuredCloneWriteInfo;

class IDBObjectStore MOZ_FINAL
  : public nsISupports
  , public nsWrapperCache
{
  nsRefPtr<IDBTransaction> mTransaction;
  JS::Heap<JS::Value> mCachedKeyPath;

  // This normally points to the ObjectStoreSpec owned by the parent IDBDatabase
  // object. However, if this objectStore is part of a versionchange transaction
  // and it gets deleted then the spec is copied into mDeletedSpec and mSpec is
  // set to point at mDeletedSpec.
  const ObjectStoreSpec* mSpec;
  nsAutoPtr<ObjectStoreSpec> mDeletedSpec;

  nsTArray<nsRefPtr<IDBIndex>> mIndexes;

  bool mRooted;

public:
  NS_DECL_CYCLE_COLLECTING_ISUPPORTS
  NS_DECL_CYCLE_COLLECTION_SCRIPT_HOLDER_CLASS(IDBObjectStore)

  static already_AddRefed<IDBObjectStore>
  Create(IDBTransaction* aTransaction, const ObjectStoreSpec& aSpec);

  static nsresult
  AppendIndexUpdateInfo(int64_t aIndexID,
                        const KeyPath& aKeyPath,
                        bool aUnique,
                        bool aMultiEntry,
                        JSContext* aCx,
                        JS::Handle<JS::Value> aObject,
                        nsTArray<IndexUpdateInfo>& aUpdateInfoArray);

  static nsresult
  UpdateIndexes(IDBTransaction* aTransaction,
                int64_t aObjectStoreId,
                const Key& aObjectStoreKey,
                bool aOverwrite,
                int64_t aObjectDataId,
                const nsTArray<IndexUpdateInfo>& aUpdateInfoArray);

  static nsresult
  GetStructuredCloneReadInfoFromStatement(mozIStorageStatement* aStatement,
                                          uint32_t aDataIndex,
                                          uint32_t aFileIdsIndex,
                                          IDBDatabase* aDatabase,
                                          StructuredCloneReadInfo& aInfo);

  static void
  ClearCloneReadInfo(StructuredCloneReadInfo& aReadInfo);

  static void
  ClearCloneWriteInfo(StructuredCloneWriteInfo& aWriteInfo);

  static bool
  DeserializeValue(JSContext* aCx,
                   StructuredCloneReadInfo& aCloneReadInfo,
                   JS::MutableHandle<JS::Value> aValue);

  static bool
  SerializeValue(JSContext* aCx,
                 StructuredCloneWriteInfo& aCloneWriteInfo,
                 JS::Handle<JS::Value> aValue);

  static bool
  DeserializeIndexValue(JSContext* aCx,
                        StructuredCloneReadInfo& aCloneReadInfo,
                        JS::MutableHandle<JS::Value> aValue);

  static bool
  StructuredCloneWriteCallback(JSContext* aCx,
                               JSStructuredCloneWriter* aWriter,
                               JS::Handle<JSObject*> aObj,
                               void* aClosure);

  static nsresult
  ConvertFileIdsToArray(const nsAString& aFileIds,
                        nsTArray<int64_t>& aResult);

  // Called only in the main process.
  static nsresult
  ConvertBlobsToActors(ContentParent* aContentParent,
                       FileManager* aFileManager,
                       const nsTArray<StructuredCloneFile>& aFiles,
                       nsTArray<PBlobParent*>& aActors);

  // Called only in the child process.
  static void
  ConvertActorsToBlobs(const nsTArray<PBlobChild*>& aActors,
                       nsTArray<StructuredCloneFile>& aFiles);

  const nsString&
  Name() const;

  bool
  AutoIncrement() const;

  bool
  IsWriteAllowed() const;

  int64_t
  Id() const;

  const KeyPath&
  GetKeyPath() const;

  bool
  HasValidKeyPath() const;

  ObjectStoreInfo*
  Info()
  {
    MOZ_CRASH("Remove");
    return nullptr;
  }

  nsresult AddOrPutInternal(
                      const SerializedStructuredCloneWriteInfo& aCloneWriteInfo,
                      const Key& aKey,
                      const nsTArray<IndexUpdateInfo>& aUpdateInfoArray,
                      const nsTArray<nsCOMPtr<nsIDOMBlob> >& aBlobs,
                      bool aOverwrite,
                      IDBRequest** _retval);

  already_AddRefed<IDBRequest>
  GetAllInternal(IDBKeyRange* aKeyRange,
                 uint32_t aLimit,
                 ErrorResult& aRv);

  already_AddRefed<IDBRequest>
  GetAllKeysInternal(IDBKeyRange* aKeyRange,
                     uint32_t aLimit,
                     ErrorResult& aRv);

  already_AddRefed<IDBRequest>
  OpenCursorInternal(IDBKeyRange* aKeyRange,
                     size_t aDirection,
                     ErrorResult& aRv);

  already_AddRefed<IDBRequest>
  OpenKeyCursorInternal(IDBKeyRange* aKeyRange,
                        size_t aDirection,
                        ErrorResult& aRv);

  nsresult
  OpenCursorFromChildProcess(
                            IDBRequest* aRequest,
                            size_t aDirection,
                            const Key& aKey,
                            const SerializedStructuredCloneReadInfo& aCloneInfo,
                            nsTArray<StructuredCloneFile>& aBlobs,
                            IDBCursor** _retval);

  nsresult
  OpenCursorFromChildProcess(IDBRequest* aRequest,
                             size_t aDirection,
                             const Key& aKey,
                             IDBCursor** _retval);

  void
  SetInfo(ObjectStoreInfo* aInfo)
  {
    MOZ_CRASH("Remove");
  }

  static const JSClass sDummyPropJSClass;

  // nsWrapperCache
  virtual JSObject*
  WrapObject(JSContext* aCx, JS::Handle<JSObject*> aScope) MOZ_OVERRIDE;

  // WebIDL
  nsPIDOMWindow*
  GetParentObject() const;

  void
  GetName(nsString& aName) const
  {
    aName = Name();
  }

  JS::Value
  GetKeyPath(JSContext* aCx, ErrorResult& aRv);

  already_AddRefed<DOMStringList>
  IndexNames();

  IDBTransaction*
  Transaction() const
  {
    AssertIsOnOwningThread();
    return mTransaction;
  }

  already_AddRefed<IDBRequest>
  Add(JSContext* aCx,
      JS::Handle<JS::Value> aValue,
      JS::Handle<JS::Value> aKey,
      ErrorResult& aRv)
  {
    AssertIsOnOwningThread();

    return AddOrPut(aCx, aValue, aKey, false, aRv);
  }

  already_AddRefed<IDBRequest>
  Put(JSContext* aCx,
      JS::Handle<JS::Value> aValue,
      JS::Handle<JS::Value> aKey,
      ErrorResult& aRv)
  {
    AssertIsOnOwningThread();

    return AddOrPut(aCx, aValue, aKey, true, aRv);
  }

  already_AddRefed<IDBRequest>
  Delete(JSContext* aCx, JS::Handle<JS::Value> aKey, ErrorResult& aRv);

  already_AddRefed<IDBRequest>
  Get(JSContext* aCx, JS::Handle<JS::Value> aKey, ErrorResult& aRv);

  already_AddRefed<IDBRequest>
  Clear(ErrorResult& aRv);

  already_AddRefed<IDBRequest>
  OpenCursor(JSContext* aCx, JS::Handle<JS::Value> aRange,
             IDBCursorDirection aDirection, ErrorResult& aRv);

  already_AddRefed<IDBIndex>
  CreateIndex(JSContext* aCx, const nsAString& aName, const nsAString& aKeyPath,
              const IDBIndexParameters& aOptionalParameters, ErrorResult& aRv);

  already_AddRefed<IDBIndex>
  CreateIndex(JSContext* aCx, const nsAString& aName,
              const Sequence<nsString>& aKeyPath,
              const IDBIndexParameters& aOptionalParameters, ErrorResult& aRv);

  already_AddRefed<IDBIndex>
  Index(const nsAString& aName, ErrorResult &aRv);

  void
  DeleteIndex(const nsAString& aIndexName, ErrorResult& aRv);

  already_AddRefed<IDBRequest>
  Count(JSContext* aCx, JS::Handle<JS::Value> aKey,
        ErrorResult& aRv);

  already_AddRefed<IDBRequest>
  GetAll(JSContext* aCx, JS::Handle<JS::Value> aKey,
         const Optional<uint32_t>& aLimit, ErrorResult& aRv);

  already_AddRefed<IDBRequest>
  GetAllKeys(JSContext* aCx, JS::Handle<JS::Value> aKey,
             const Optional<uint32_t>& aLimit, ErrorResult& aRv);

  already_AddRefed<IDBRequest>
  OpenKeyCursor(JSContext* aCx, JS::Handle<JS::Value> aRange,
                IDBCursorDirection aDirection, ErrorResult& aRv);

  void
  AssertIsOnOwningThread() const
#ifdef DEBUG
  ;
#else
  { }
#endif

  void
  RefreshSpec();

  const ObjectStoreSpec&
  Spec() const;

  void
  NoteDeletion();

private:
  IDBObjectStore();
  ~IDBObjectStore();

  nsresult
  GetAddInfo(JSContext* aCx,
             JS::Handle<JS::Value> aValue,
             JS::Handle<JS::Value> aKeyVal,
             StructuredCloneWriteInfo& aCloneWriteInfo,
             Key& aKey,
             nsTArray<IndexUpdateInfo>& aUpdateInfoArray);

  already_AddRefed<IDBRequest>
  AddOrPut(JSContext* aCx,
           JS::Handle<JS::Value> aValue,
           JS::Handle<JS::Value> aKey,
           bool aOverwrite,
           ErrorResult& aRv);

  already_AddRefed<IDBIndex>
  CreateIndexInternal(JSContext* aCx,
                      const nsAString& aName,
                      const KeyPath& aKeyPath,
                      const IDBIndexParameters& aOptionalParameters,
                      ErrorResult& aRv);

  static void
  ClearStructuredCloneBuffer(JSAutoStructuredCloneBuffer& aBuffer);
};

} // namespace indexedDB
} // namespace dom
} // namespace mozilla

#endif // mozilla_dom_indexeddb_idbobjectstore_h__
