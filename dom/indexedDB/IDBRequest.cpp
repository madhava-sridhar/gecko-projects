/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "IDBRequest.h"

#include "BackgroundChildImpl.h"
#include "IDBCursor.h"
#include "IDBDatabase.h"
#include "IDBEvents.h"
#include "IDBFactory.h"
#include "IDBIndex.h"
#include "IDBObjectStore.h"
#include "IDBTransaction.h"
#include "mozilla/ContentEvents.h"
#include "mozilla/ErrorResult.h"
#include "mozilla/EventDispatcher.h"
#include "mozilla/dom/ErrorEventBinding.h"
#include "mozilla/dom/IDBOpenDBRequestBinding.h"
#include "mozilla/dom/UnionTypes.h"
#include "nsCOMPtr.h"
#include "nsContentUtils.h"
#include "nsIScriptContext.h"
#include "nsJSUtils.h"
#include "nsPIDOMWindow.h"
#include "nsString.h"
#include "ReportInternalError.h"

using namespace mozilla;
using namespace mozilla::dom;
using namespace mozilla::dom::indexedDB;
using namespace mozilla::ipc;

IDBRequest::IDBRequest(IDBDatabase* aDatabase)
  : IDBWrapperCache(aDatabase)
{
  MOZ_ASSERT(aDatabase);
  aDatabase->AssertIsOnOwningThread();

  InitMembers();
}

IDBRequest::IDBRequest(nsPIDOMWindow* aOwner)
  : IDBWrapperCache(aOwner)
{
  InitMembers();
}

IDBRequest::~IDBRequest()
{
  AssertIsOnOwningThread();
}

#ifdef DEBUG

void
IDBRequest::AssertIsOnOwningThread() const
{
  MOZ_ASSERT(mOwningThread);
  MOZ_ASSERT(PR_GetCurrentThread() == mOwningThread);
}

#endif // DEBUG

void
IDBRequest::InitMembers()
{
#ifdef DEBUG
  mOwningThread = PR_GetCurrentThread();
#endif
  AssertIsOnOwningThread();

  mResultVal.setUndefined();
  mErrorCode = NS_OK;
  mLineNo = 0;
  mHaveResultOrErrorCode = false;

#ifdef MOZ_ENABLE_PROFILER_SPS
  // XXX This should be threadlocal!
  /*
  BackgroundChildImpl::ThreadLocal* threadLocal =
    BackgroundChildImpl::GetThreadLocalForCurrentThread();
  MOZ_ASSERT(threadLocal);

  mSerialNumber = threadLocal->mNextRequestSerialNumber++;
  */
  static uint64_t sNextSerial = 1;
  mSerialNumber = sNextSerial++;
#endif
}

// static
already_AddRefed<IDBRequest>
IDBRequest::Create(IDBDatabase* aDatabase,
                   IDBTransaction* aTransaction)
{
  MOZ_ASSERT(aDatabase);
  aDatabase->AssertIsOnOwningThread();

  nsRefPtr<IDBRequest> request = new IDBRequest(aDatabase);

  request->mTransaction = aTransaction;
  request->SetScriptOwner(aDatabase->GetScriptOwner());

  request->CaptureCaller();

  return request.forget();
}

// static
already_AddRefed<IDBRequest>
IDBRequest::Create(IDBObjectStore* aSourceAsObjectStore,
                   IDBDatabase* aDatabase,
                   IDBTransaction* aTransaction)
{
  MOZ_ASSERT(aSourceAsObjectStore);
  aSourceAsObjectStore->AssertIsOnOwningThread();

  nsRefPtr<IDBRequest> request = Create(aDatabase, aTransaction);

  request->mSourceAsObjectStore = aSourceAsObjectStore;

  return request.forget();
}

// static
already_AddRefed<IDBRequest>
IDBRequest::Create(IDBIndex* aSourceAsIndex,
                   IDBDatabase* aDatabase,
                   IDBTransaction* aTransaction)
{
  MOZ_ASSERT(aSourceAsIndex);
  aSourceAsIndex->AssertIsOnOwningThread();

  nsRefPtr<IDBRequest> request = Create(aDatabase, aTransaction);

  request->mSourceAsIndex = aSourceAsIndex;

  return request.forget();
}

void
IDBRequest::GetSource(
             Nullable<OwningIDBObjectStoreOrIDBIndexOrIDBCursor>& aSource) const
{
  AssertIsOnOwningThread();

  // At most one of mSourceAs* is allowed to be non-null.  Check that by summing
  // the double negation of each one and asserting the sum is at most 1.
  MOZ_ASSERT(!!mSourceAsObjectStore + !!mSourceAsIndex + !!mSourceAsCursor <=
             1);

  if (mSourceAsObjectStore) {
    aSource.SetValue().SetAsIDBObjectStore() = mSourceAsObjectStore;
  } else if (mSourceAsIndex) {
    aSource.SetValue().SetAsIDBIndex() = mSourceAsIndex;
  } else if (mSourceAsCursor) {
    aSource.SetValue().SetAsIDBCursor() = mSourceAsCursor;
  } else {
    aSource.SetNull();
  }
}

void
IDBRequest::Reset()
{
  AssertIsOnOwningThread();

  mResultVal.setUndefined();
  mHaveResultOrErrorCode = false;
  mError = nullptr;
}

void
IDBRequest::DispatchNonTransactionError(nsresult aErrorCode)
{
  AssertIsOnOwningThread();
  MOZ_ASSERT(NS_FAILED(aErrorCode));
  MOZ_ASSERT(NS_ERROR_GET_MODULE(aErrorCode) == NS_ERROR_MODULE_DOM_INDEXEDDB);

  SetError(aErrorCode);

  // Make an error event and fire it at the target.
  nsCOMPtr<nsIDOMEvent> event =
    CreateGenericEvent(this,
                       nsDependentString(kErrorEventType),
                       eDoesBubble,
                       eCancelable);
  if (NS_WARN_IF(!event)) {
    return;
  }

  bool ignored;
  NS_WARN_IF(NS_FAILED(DispatchEvent(event, &ignored)));
}

void
IDBRequest::SetError(nsresult aRv)
{
  AssertIsOnOwningThread();
  MOZ_ASSERT(NS_FAILED(aRv));
  MOZ_ASSERT(NS_ERROR_GET_MODULE(aRv) == NS_ERROR_MODULE_DOM_INDEXEDDB);
  MOZ_ASSERT(!mError);

  mHaveResultOrErrorCode = true;
  mError = new DOMError(GetOwner(), aRv);
  mErrorCode = aRv;

  mResultVal.setUndefined();
}

#ifdef DEBUG

nsresult
IDBRequest::GetErrorCode() const
{
  AssertIsOnOwningThread();
  MOZ_ASSERT(mHaveResultOrErrorCode);

  return mErrorCode;
}

#endif // DEBUG

JSContext*
IDBRequest::GetJSContext()
{
  AssertIsOnOwningThread();

  JSContext* cx;

  // XXX Fix me!
  MOZ_ASSERT(NS_IsMainThread(), "This can't work off the main thread!");
  if (GetScriptOwner()) {
    return nsContentUtils::GetSafeJSContext();
  }

  nsresult rv;
  nsIScriptContext* sc = GetContextForEventHandlers(&rv);
  NS_ENSURE_SUCCESS(rv, nullptr);
  NS_ENSURE_TRUE(sc, nullptr);

  cx = sc->GetNativeContext();
  NS_ASSERTION(cx, "Failed to get a context!");

  return cx;
}

void
IDBRequest::CaptureCaller()
{
  AutoJSContext cx;

  const char* filename = nullptr;
  uint32_t lineNo = 0;
  if (NS_WARN_IF(!nsJSUtils::GetCallingLocation(cx, &filename, &lineNo))) {
    return;
  }

  mFilename.Assign(NS_ConvertUTF8toUTF16(filename));
  mLineNo = lineNo;
}

void
IDBRequest::FillScriptErrorEvent(ErrorEventInit& aEventInit) const
{
  aEventInit.mLineno = mLineNo;
  aEventInit.mFilename = mFilename;
}

IDBRequestReadyState
IDBRequest::ReadyState() const
{
  AssertIsOnOwningThread();

  return IsPending() ?
    IDBRequestReadyState::Pending :
    IDBRequestReadyState::Done;
}

void
IDBRequest::SetSource(IDBCursor* aSource)
{
  AssertIsOnOwningThread();
  MOZ_ASSERT(aSource);
  MOZ_ASSERT(mSourceAsObjectStore || mSourceAsIndex);
  MOZ_ASSERT(!mSourceAsCursor);

  mSourceAsCursor = aSource;
  mSourceAsObjectStore = nullptr;
  mSourceAsIndex = nullptr;
}

JSObject*
IDBRequest::WrapObject(JSContext* aCx)
{
  return IDBRequestBinding::Wrap(aCx, this);
}

JS::Value
IDBRequest::GetResult(ErrorResult& aRv) const
{
  AssertIsOnOwningThread();

  if (!mHaveResultOrErrorCode) {
    aRv.Throw(NS_ERROR_DOM_INVALID_STATE_ERR);
    return JS::UndefinedValue();
  }

  return mResultVal;
}

void
IDBRequest::SetResult(GetResultCallback aCallback, void* aUserData)
{
  AssertIsOnOwningThread();
  MOZ_ASSERT(aCallback);
  MOZ_ASSERT(!mHaveResultOrErrorCode);
  MOZ_ASSERT(mResultVal.isUndefined());
  MOZ_ASSERT(!mError);

  // See if our window is still valid.
  if (NS_WARN_IF(NS_FAILED(CheckInnerWindowCorrectness()))) {
    IDB_REPORT_INTERNAL_ERR();
    SetError(NS_ERROR_DOM_INDEXEDDB_UNKNOWN_ERR);
    return;
  }

  JSContext* cx = GetJSContext();
  if (NS_WARN_IF(!cx)) {
    IDB_REPORT_INTERNAL_ERR();
    SetError(NS_ERROR_DOM_INDEXEDDB_UNKNOWN_ERR);
    return;
  }

  Maybe<AutoPushJSContext> maybePush;
  if (NS_IsMainThread()) {
    maybePush.construct(cx);
  }

  JS::Rooted<JSObject*> global(cx, IDBWrapperCache::GetParentObject());
  MOZ_ASSERT(global);

  JSAutoCompartment ac(cx, global);
  AssertIsRooted();

  JS::Rooted<JS::Value> result(cx);
  nsresult rv = aCallback(cx, aUserData, &result);
  if (NS_WARN_IF(NS_FAILED(rv))) {
    SetError(rv);
    mResultVal.setUndefined();
  } else {
    mError = nullptr;
    mResultVal = result;
  }

  mHaveResultOrErrorCode = true;
}

mozilla::dom::DOMError*
IDBRequest::GetError(mozilla::ErrorResult& aRv)
{
  AssertIsOnOwningThread();

  if (!mHaveResultOrErrorCode) {
    aRv.Throw(NS_ERROR_DOM_INVALID_STATE_ERR);
    return nullptr;
  }

  return mError;
}

NS_IMPL_CYCLE_COLLECTION_CLASS(IDBRequest)

NS_IMPL_CYCLE_COLLECTION_TRAVERSE_BEGIN_INHERITED(IDBRequest, IDBWrapperCache)
  // Don't need NS_IMPL_CYCLE_COLLECTION_TRAVERSE_SCRIPT_OBJECTS because
  // DOMEventTargetHelper does it for us.
  NS_IMPL_CYCLE_COLLECTION_TRAVERSE(mSourceAsObjectStore)
  NS_IMPL_CYCLE_COLLECTION_TRAVERSE(mSourceAsIndex)
  NS_IMPL_CYCLE_COLLECTION_TRAVERSE(mSourceAsCursor)
  NS_IMPL_CYCLE_COLLECTION_TRAVERSE(mTransaction)
  NS_IMPL_CYCLE_COLLECTION_TRAVERSE(mError)
NS_IMPL_CYCLE_COLLECTION_TRAVERSE_END

NS_IMPL_CYCLE_COLLECTION_UNLINK_BEGIN_INHERITED(IDBRequest, IDBWrapperCache)
  tmp->mResultVal.setUndefined();
  NS_IMPL_CYCLE_COLLECTION_UNLINK(mSourceAsObjectStore)
  NS_IMPL_CYCLE_COLLECTION_UNLINK(mSourceAsIndex)
  NS_IMPL_CYCLE_COLLECTION_UNLINK(mSourceAsCursor)
  NS_IMPL_CYCLE_COLLECTION_UNLINK(mTransaction)
  NS_IMPL_CYCLE_COLLECTION_UNLINK(mError)
NS_IMPL_CYCLE_COLLECTION_UNLINK_END

NS_IMPL_CYCLE_COLLECTION_TRACE_BEGIN_INHERITED(IDBRequest, IDBWrapperCache)
  // Don't need NS_IMPL_CYCLE_COLLECTION_TRACE_PRESERVED_WRAPPER because
  // DOMEventTargetHelper does it for us.
  NS_IMPL_CYCLE_COLLECTION_TRACE_JSVAL_MEMBER_CALLBACK(mResultVal)
NS_IMPL_CYCLE_COLLECTION_TRACE_END

NS_INTERFACE_MAP_BEGIN_CYCLE_COLLECTION_INHERITED(IDBRequest)
NS_INTERFACE_MAP_END_INHERITING(IDBWrapperCache)

NS_IMPL_ADDREF_INHERITED(IDBRequest, IDBWrapperCache)
NS_IMPL_RELEASE_INHERITED(IDBRequest, IDBWrapperCache)

nsresult
IDBRequest::PreHandleEvent(EventChainPreVisitor& aVisitor)
{
  AssertIsOnOwningThread();

  aVisitor.mCanHandle = true;
  aVisitor.mParentTarget = mTransaction;
  return NS_OK;
}

IDBOpenDBRequest::IDBOpenDBRequest(nsPIDOMWindow* aOwner)
  : IDBRequest(aOwner)
{
  AssertIsOnOwningThread();
}

IDBOpenDBRequest::~IDBOpenDBRequest()
{
  AssertIsOnOwningThread();
}

// static
already_AddRefed<IDBOpenDBRequest>
IDBOpenDBRequest::Create(IDBFactory* aFactory,
                         nsPIDOMWindow* aOwner,
                         JS::Handle<JSObject*> aScriptOwner)
{
  MOZ_ASSERT(aFactory);
  aFactory->AssertIsOnOwningThread();

  nsRefPtr<IDBOpenDBRequest> request = new IDBOpenDBRequest(aOwner);

  request->SetScriptOwner(aScriptOwner);
  request->mFactory = aFactory;

  request->CaptureCaller();

  return request.forget();
}

void
IDBOpenDBRequest::SetTransaction(IDBTransaction* aTransaction)
{
  AssertIsOnOwningThread();

  MOZ_ASSERT(!aTransaction || !mTransaction);

  mTransaction = aTransaction;
}

NS_IMPL_CYCLE_COLLECTION_CLASS(IDBOpenDBRequest)

NS_IMPL_CYCLE_COLLECTION_TRAVERSE_BEGIN_INHERITED(IDBOpenDBRequest,
                                                  IDBRequest)
  NS_IMPL_CYCLE_COLLECTION_TRAVERSE(mFactory)
NS_IMPL_CYCLE_COLLECTION_TRAVERSE_END

NS_IMPL_CYCLE_COLLECTION_UNLINK_BEGIN_INHERITED(IDBOpenDBRequest,
                                                IDBRequest)
  // Don't unlink mFactory!
NS_IMPL_CYCLE_COLLECTION_UNLINK_END

NS_INTERFACE_MAP_BEGIN_CYCLE_COLLECTION_INHERITED(IDBOpenDBRequest)
NS_INTERFACE_MAP_END_INHERITING(IDBRequest)

NS_IMPL_ADDREF_INHERITED(IDBOpenDBRequest, IDBRequest)
NS_IMPL_RELEASE_INHERITED(IDBOpenDBRequest, IDBRequest)

nsresult
IDBOpenDBRequest::PostHandleEvent(EventChainPostVisitor& aVisitor)
{
  // XXX Fix me!
  MOZ_ASSERT(NS_IsMainThread());

  return IndexedDatabaseManager::FireWindowOnError(GetOwner(), aVisitor);
}

JSObject*
IDBOpenDBRequest::WrapObject(JSContext* aCx)
{
  AssertIsOnOwningThread();

  return IDBOpenDBRequestBinding::Wrap(aCx, this);
}
