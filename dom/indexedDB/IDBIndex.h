/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_dom_indexeddb_idbindex_h__
#define mozilla_dom_indexeddb_idbindex_h__

#include "js/RootingAPI.h"
#include "mozilla/Attributes.h"
#include "mozilla/dom/IDBCursorBinding.h"
#include "nsAutoPtr.h"
#include "nsCycleCollectionParticipant.h"
#include "nsTArrayForwardDeclare.h"
#include "nsWrapperCache.h"

class nsIScriptContext;
class nsPIDOMWindow;

namespace mozilla {

class ErrorResult;

namespace dom {

template <typename> class Sequence;

namespace indexedDB {

class AsyncConnectionHelper;
class IDBCursor;
class IDBKeyRange;
class IDBObjectStore;
class IDBRequest;
class IndexedDBIndexChild;
class IndexedDBIndexParent;
struct IndexInfo;
class IndexMetadata;
class Key;
class KeyPath;
struct SerializedStructuredCloneReadInfo;
struct StructuredCloneFile;

class IDBIndex MOZ_FINAL
  : public nsISupports
  , public nsWrapperCache
{
  nsRefPtr<IDBObjectStore> mObjectStore;

  JS::Heap<JS::Value> mCachedKeyPath;

  nsAutoPtr<IndexMetadata> mMetadata;

  IndexedDBIndexChild* mActorChild;
  IndexedDBIndexParent* mActorParent;

  bool mRooted;

public:
  NS_DECL_CYCLE_COLLECTING_ISUPPORTS
  NS_DECL_CYCLE_COLLECTION_SCRIPT_HOLDER_CLASS(IDBIndex)

  static already_AddRefed<IDBIndex>
  Create(IDBObjectStore* aObjectStore,
         const IndexInfo* aIndexInfo,
         bool aCreating);

  static already_AddRefed<IDBIndex>
  Create(IDBObjectStore* aObjectStore, const IndexMetadata& aMetadata);

  int64_t
  Id() const;

  const nsString&
  Name() const;

  bool
  Unique() const;

  bool
  MultiEntry() const;

  const KeyPath&
  GetKeyPath() const;

  IDBObjectStore*
  ObjectStore() const
  {
    AssertIsOnOwningThread();
    return mObjectStore;
  }

  void
  SetActor(IndexedDBIndexChild* aActorChild)
  {
    NS_ASSERTION(!aActorChild || !mActorChild, "Shouldn't have more than one!");
    mActorChild = aActorChild;
  }

  void
  SetActor(IndexedDBIndexParent* aActorParent)
  {
    NS_ASSERTION(!aActorParent || !mActorParent,
                 "Shouldn't have more than one!");
    mActorParent = aActorParent;
  }

  IndexedDBIndexChild*
  GetActorChild() const
  {
    return mActorChild;
  }

  IndexedDBIndexParent*
  GetActorParent() const
  {
    return mActorParent;
  }

  already_AddRefed<IDBRequest>
  GetInternal(IDBKeyRange* aKeyRange,
              ErrorResult& aRv);

  already_AddRefed<IDBRequest>
  GetKeyInternal(IDBKeyRange* aKeyRange,
                 ErrorResult& aRv);

  already_AddRefed<IDBRequest>
  GetAllInternal(IDBKeyRange* aKeyRange,
                 uint32_t aLimit,
                 ErrorResult& aRv);

  already_AddRefed<IDBRequest>
  GetAllKeysInternal(IDBKeyRange* aKeyRange,
                     uint32_t aLimit,
                     ErrorResult& aRv);

  already_AddRefed<IDBRequest>
  CountInternal(IDBKeyRange* aKeyRange,
                ErrorResult& aRv);

  nsresult OpenCursorFromChildProcess(
                            IDBRequest* aRequest,
                            size_t aDirection,
                            const Key& aKey,
                            const Key& aObjectKey,
                            IDBCursor** _retval);

  already_AddRefed<IDBRequest>
  OpenKeyCursorInternal(IDBKeyRange* aKeyRange,
                        size_t aDirection,
                        ErrorResult& aRv);

  nsresult OpenCursorInternal(IDBKeyRange* aKeyRange,
                              size_t aDirection,
                              IDBRequest** _retval);

  nsresult OpenCursorFromChildProcess(
                            IDBRequest* aRequest,
                            size_t aDirection,
                            const Key& aKey,
                            const Key& aObjectKey,
                            const SerializedStructuredCloneReadInfo& aCloneInfo,
                            nsTArray<StructuredCloneFile>& aBlobs,
                            IDBCursor** _retval);

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

  already_AddRefed<IDBRequest>
  OpenCursor(JSContext* aCx, JS::Handle<JS::Value> aRange,
             IDBCursorDirection aDirection, ErrorResult& aRv);

  already_AddRefed<IDBRequest>
  OpenKeyCursor(JSContext* aCx, JS::Handle<JS::Value> aRange,
                IDBCursorDirection aDirection, ErrorResult& aRv);

  already_AddRefed<IDBRequest>
  Get(JSContext* aCx, JS::Handle<JS::Value> aKey, ErrorResult& aRv);

  already_AddRefed<IDBRequest>
  GetKey(JSContext* aCx, JS::Handle<JS::Value> aKey, ErrorResult& aRv);

  already_AddRefed<IDBRequest>
  Count(JSContext* aCx, JS::Handle<JS::Value> aKey,
         ErrorResult& aRv);

  void
  GetStoreName(nsString& aStoreName) const;

  already_AddRefed<IDBRequest>
  GetAll(JSContext* aCx, JS::Handle<JS::Value> aKey,
         const Optional<uint32_t>& aLimit, ErrorResult& aRv);

  already_AddRefed<IDBRequest>
  GetAllKeys(JSContext* aCx, JS::Handle<JS::Value> aKey,
             const Optional<uint32_t>& aLimit, ErrorResult& aRv);

  void
  AssertIsOnOwningThread() const
#ifdef DEBUG
  ;
#else
  { }
#endif

private:
  IDBIndex();
  ~IDBIndex();
};

} // namespace indexedDB
} // namespace dom
} // namespace mozilla

#endif // mozilla_dom_indexeddb_idbindex_h__
