/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_dom_indexeddb_idbfactory_h__
#define mozilla_dom_indexeddb_idbfactory_h__

#include "mozilla/Attributes.h"
#include "mozilla/dom/BindingDeclarations.h" // for Optional
#include "mozilla/dom/StorageTypeBinding.h"
#include "mozilla/dom/quota/PersistenceType.h"
#include "mozilla/dom/quota/StoragePrivilege.h"
#include "nsAutoPtr.h"
#include "nsCOMPtr.h"
#include "nsCycleCollectionParticipant.h"
#include "nsISupports.h"
#include "nsString.h"
#include "nsTArray.h"
#include "nsWrapperCache.h"

class mozIStorageConnection;
class nsIFile;
class nsIFileURL;
class nsIPrincipal;
class nsPIDOMWindow;
struct PRThread;

namespace mozilla {

class ErrorResult;

namespace ipc {

class PBackgroundChild;

} // namespace ipc

namespace dom {

class ContentParent;
class IDBOpenDBOptions;

namespace indexedDB {

class BackgroundFactoryChild;
class FactoryRequestParams;
class IDBDatabase;
class IDBOpenDBRequest;

class IDBFactory MOZ_FINAL
  : public nsISupports
  , public nsWrapperCache
{
  typedef mozilla::dom::ContentParent ContentParent;
  typedef mozilla::dom::quota::PersistenceType PersistenceType;
  typedef mozilla::dom::quota::StoragePrivilege StoragePrivilege;
  typedef mozilla::ipc::PBackgroundChild PBackgroundChild;

  class BackgroundCreateCallback;
  struct PendingRequestInfo;

  nsCString mGroup;
  nsCString mASCIIOrigin;
  StoragePrivilege mPrivilege;
  PersistenceType mDefaultPersistenceType;

  // If this factory lives on a window then mWindow must be non-null. Otherwise
  // mOwningObject must be non-null.
  nsCOMPtr<nsPIDOMWindow> mWindow;
  JS::Heap<JSObject*> mOwningObject;

  nsTArray<nsAutoPtr<PendingRequestInfo>> mPendingRequests;

  BackgroundFactoryChild* mBackgroundActor;

#ifdef DEBUG
  PRThread* mOwningThread;
#endif

  bool mRootedOwningObject;
  bool mBackgroundActorFailed;

public:
  // Called when using IndexedDB from a window in a different process.
  static nsresult Create(nsPIDOMWindow* aWindow,
                         const nsACString& aGroup,
                         const nsACString& aASCIIOrigin,
                         ContentParent* aContentParent,
                         IDBFactory** aFactory);

  // Called when using IndexedDB from a window in the current process.
  static nsresult Create(nsPIDOMWindow* aWindow,
                         ContentParent* aContentParent,
                         IDBFactory** aFactory)
  {
    return Create(aWindow, EmptyCString(), EmptyCString(), aContentParent,
                  aFactory);
  }

  // Called when using IndexedDB from a JS component or a JSM in the current
  // process.
  static nsresult Create(JSContext* aCx,
                         JS::Handle<JSObject*> aOwningObject,
                         ContentParent* aContentParent,
                         IDBFactory** aFactory);

  // Called when using IndexedDB from a JS component or a JSM in a different
  // process or from a C++ component.
  static nsresult Create(ContentParent* aContentParent,
                         IDBFactory** aFactory);

  void
  AssertIsOnOwningThread() const
#ifdef DEBUG
  ;
#else
  { }
#endif

  nsresult
  OpenInternal(const nsAString& aName,
               int64_t aVersion,
               PersistenceType aPersistenceType,
               const nsACString& aGroup,
               const nsACString& aASCIIOrigin,
               StoragePrivilege aStoragePrivilege,
               bool aDeleting,
               IDBOpenDBRequest** _retval);

  nsresult
  OpenInternal(const nsAString& aName,
               int64_t aVersion,
               PersistenceType aPersistenceType,
               bool aDeleting,
               IDBOpenDBRequest** _retval)
  {
    return OpenInternal(aName, aVersion, aPersistenceType, mGroup, mASCIIOrigin,
                        mPrivilege, aDeleting, _retval);
  }

  void
  SetBackgroundActor(BackgroundFactoryChild* aBackgroundActor);

  void
  ClearBackgroundActor()
  {
    AssertIsOnOwningThread();

    mBackgroundActor = nullptr;
  }

  const nsCString&
  GetASCIIOrigin() const
  {
    return mASCIIOrigin;
  }

  nsPIDOMWindow*
  GetParentObject() const
  {
    return mWindow;
  }

  already_AddRefed<IDBOpenDBRequest>
  Open(const nsAString& aName, uint64_t aVersion, ErrorResult& aRv)
  {
    return Open(nullptr, aName, Optional<uint64_t>(aVersion),
                Optional<mozilla::dom::StorageType>(), false, aRv);
  }

  already_AddRefed<IDBOpenDBRequest>
  Open(const nsAString& aName,
       const IDBOpenDBOptions& aOptions,
       ErrorResult& aRv);

  already_AddRefed<IDBOpenDBRequest>
  DeleteDatabase(const nsAString& aName,
                 const IDBOpenDBOptions& aOptions,
                 ErrorResult& aRv);

  int16_t
  Cmp(JSContext* aCx,
      JS::Handle<JS::Value> aFirst,
      JS::Handle<JS::Value> aSecond,
      ErrorResult& aRv);

  already_AddRefed<IDBOpenDBRequest>
  OpenForPrincipal(nsIPrincipal* aPrincipal,
                   const nsAString& aName,
                   uint64_t aVersion,
                   ErrorResult& aRv);

  already_AddRefed<IDBOpenDBRequest>
  OpenForPrincipal(nsIPrincipal* aPrincipal,
                   const nsAString& aName,
                   const IDBOpenDBOptions& aOptions,
                   ErrorResult& aRv);

  already_AddRefed<IDBOpenDBRequest>
  DeleteForPrincipal(nsIPrincipal* aPrincipal,
                     const nsAString& aName,
                     const IDBOpenDBOptions& aOptions,
                     ErrorResult& aRv);

  NS_DECL_CYCLE_COLLECTING_ISUPPORTS
  NS_DECL_CYCLE_COLLECTION_SCRIPT_HOLDER_CLASS(IDBFactory)

  // nsWrapperCache
  virtual JSObject*
  WrapObject(JSContext* aCx) MOZ_OVERRIDE;

private:
  IDBFactory();
  ~IDBFactory();

  already_AddRefed<IDBOpenDBRequest>
  Open(nsIPrincipal* aPrincipal,
       const nsAString& aName,
       const Optional<uint64_t>& aVersion,
       const Optional<mozilla::dom::StorageType>& aStorageType,
       bool aDelete,
       ErrorResult& aRv);

  nsresult
  BackgroundActorCreated(PBackgroundChild* aBackgroundActor);

  void
  BackgroundActorFailed();

  nsresult
  InitiateRequest(IDBOpenDBRequest* aRequest,
                  const FactoryRequestParams& aParams);
};

} // namespace indexedDB
} // namespace dom
} // namespace mozilla

#endif // mozilla_dom_indexeddb_idbfactory_h__
