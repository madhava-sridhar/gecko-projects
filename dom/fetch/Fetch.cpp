/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "Fetch.h"

#include "nsIDocument.h"
#include "nsIGlobalObject.h"

#include "mozilla/dom/Promise.h"
#include "mozilla/dom/PromiseWorkerProxy.h"
#include "mozilla/dom/Request.h"
#include "mozilla/dom/Response.h"
#include "mozilla/dom/ScriptSettings.h"
#include "mozilla/dom/WorkerScope.h"
#include "mozilla/dom/workers/Workers.h"

#include "InternalResponse.h"
#include "WorkerPrivate.h"
#include "WorkerRunnable.h"

namespace mozilla {
namespace dom {

using namespace workers;

class MainThreadFetchRunnable : public nsRunnable
{
  nsRefPtr<WorkerPromiseHolder> mPromiseHolder;
  nsRefPtr<InternalRequest> mRequest;

public:
  MainThreadFetchRunnable(WorkerPrivate* aWorkerPrivate,
                          Promise* aPromise,
                          InternalRequest* aRequest)
    : mPromiseHolder(new WorkerPromiseHolder(aWorkerPrivate, aPromise))
    , mRequest(aRequest)
  {
    MOZ_ASSERT(aWorkerPrivate);
    aWorkerPrivate->AssertIsOnWorkerThread();
  }

  nsresult
  Run()
  {
    AssertIsOnMainThread();
    nsRefPtr<WorkerResolveFetchWithResponse> resolver =
      new WorkerResolveFetchWithResponse(mPromiseHolder);
    nsRefPtr<FetchDriver> fetch = new FetchDriver(mRequest);
    nsresult rv = fetch->Fetch(resolver);
    if (NS_FAILED(rv)) {
      // FIXME(nsm): Reject promise.
      return rv;
    }

    return NS_OK;
  }
};

already_AddRefed<Promise>
WorkerDOMFetch(nsIGlobalObject* aGlobal, const RequestOrScalarValueString& aInput,
               const RequestInit& aInit, ErrorResult& aRv)
{
  nsRefPtr<Promise> p = Promise::Create(aGlobal, aRv);
  if (aRv.Failed()) {
    return nullptr;
  }

  AutoJSAPI jsapi;
  jsapi.Init(aGlobal);
  JSContext* cx = jsapi.cx();

  JS::Rooted<JSObject*> jsGlobal(cx, aGlobal->GetGlobalJSObject());
  GlobalObject global(cx, jsGlobal);

  nsRefPtr<Request> request = Request::Constructor(global, aInput, aInit, aRv);
  if (aRv.Failed()) {
    return nullptr;
  }

  nsRefPtr<mozilla::dom::InternalRequest> r = request->GetInternalRequest();
  // Set the referrer to a URL so that it isn't attempted on the main thread.
  if (!r->ReferrerIsNone()) {
    r->SetReferrer(GetRequestReferrer(r));
  }

  nsRefPtr<MainThreadFetchRunnable> run = new MainThreadFetchRunnable(GetCurrentThreadWorkerPrivate(), p, r);
  NS_DispatchToMainThread(run);
  return p.forget();
}

already_AddRefed<Promise>
DOMFetch(nsIGlobalObject* aGlobal, const RequestOrScalarValueString& aInput,
         const RequestInit& aInit, ErrorResult& aRv)
{
  nsRefPtr<Promise> p = Promise::Create(aGlobal, aRv);
  if (aRv.Failed()) {
    return nullptr;
  }

  AutoJSAPI jsapi;
  jsapi.Init(aGlobal);
  JSContext* cx = jsapi.cx();

  JS::Rooted<JSObject*> jsGlobal(cx, aGlobal->GetGlobalJSObject());
  GlobalObject global(cx, jsGlobal);

  nsRefPtr<Request> request = Request::Constructor(global, aInput, aInit, aRv);
  if (aRv.Failed()) {
    return nullptr;
  }

  nsRefPtr<mozilla::dom::InternalRequest> r = request->GetInternalRequest();
  if (!r->ReferrerIsNone()) {
    r->SetReferrer(GetRequestReferrer(r));
  }

  nsRefPtr<ResolveFetchWithResponse> resolver = new ResolveFetchWithResponse(p);
  nsRefPtr<FetchDriver> fetch = new FetchDriver(r);
  aRv = fetch->Fetch(resolver);
  if (aRv.Failed()) {
    return nullptr;
  }

  return p.forget();
}

ResolveFetchWithResponse::ResolveFetchWithResponse(Promise* aPromise)
  : mPromise(aPromise)
{
}

void
ResolveFetchWithResponse::OnResponseAvailable(InternalResponse* aResponse)
{
  NS_ASSERT_OWNINGTHREAD(ResolveFetchWithResponse)
  AssertIsOnMainThread();
  mInternalResponse = aResponse;

  nsCOMPtr<nsIGlobalObject> go = mPromise->GetParentObject();

  mDOMResponse = new Response(go, aResponse);
  mPromise->MaybeResolve(mDOMResponse);
}

void
ResolveFetchWithResponse::OnResponseEnd()
{
  NS_ASSERT_OWNINGTHREAD(ResolveFetchWithResponse)
  AssertIsOnMainThread();
}

ResolveFetchWithResponse::~ResolveFetchWithResponse()
{
  NS_ASSERT_OWNINGTHREAD(ResolveFetchWithResponse)
}

class ResolveFetchWithResponseRunnable : public WorkerRunnable
{
  nsRefPtr<WorkerPromiseHolder> mPromiseHolder;
  nsRefPtr<InternalResponse> mResponse;
public:
  ResolveFetchWithResponseRunnable(WorkerPromiseHolder* aPromiseHolder, InternalResponse* aResponse)
    : WorkerRunnable(aPromiseHolder->GetWorkerPrivate(), WorkerThreadModifyBusyCount)
    , mPromiseHolder(aPromiseHolder)
    , mResponse(aResponse)
  {
  }

  bool
  WorkerRun(JSContext* aCx, WorkerPrivate* aWorkerPrivate) MOZ_OVERRIDE
  {
    MOZ_ASSERT(aWorkerPrivate);
    aWorkerPrivate->AssertIsOnWorkerThread();
    MOZ_ASSERT(aWorkerPrivate == mWorkerPrivate);

    MOZ_ASSERT(mPromiseHolder);
    nsRefPtr<Promise> workerPromise = mPromiseHolder->GetWorkerPromise();
    MOZ_ASSERT(workerPromise);

    nsRefPtr<nsIGlobalObject> global = aWorkerPrivate->GlobalScope();
    nsRefPtr<Response> response = new Response(global, mResponse);
    workerPromise->MaybeResolve(response);
    // Release the Promise because it has been resolved/rejected for sure.
    mPromiseHolder->CleanUp(aCx);
    return true;
  }
};

class ResolveFetchWithBodyRunnable : public WorkerRunnable
{
  nsRefPtr<InternalResponse> mResponse;
  nsRefPtr<DOMFileImpl> mFileImpl;
public:
  ResolveFetchWithBodyRunnable(WorkerPrivate* aWorkerPrivate, InternalResponse* aResponse, DOMFileImpl* aFileImpl)
    : WorkerRunnable(aWorkerPrivate, WorkerThreadModifyBusyCount)
    , mResponse(aResponse)
    , mFileImpl(aFileImpl)
  {
  }

  bool
  WorkerRun(JSContext* aCx, WorkerPrivate* aWorkerPrivate) MOZ_OVERRIDE
  {
    MOZ_ASSERT(aWorkerPrivate);
    aWorkerPrivate->AssertIsOnWorkerThread();

    mResponse->SetBody(new DOMFile(mFileImpl));
    return true;
  }
};

WorkerResolveFetchWithResponse::WorkerResolveFetchWithResponse(WorkerPromiseHolder* aHolder)
  : mPromiseHolder(aHolder)
{
}

WorkerResolveFetchWithResponse::~WorkerResolveFetchWithResponse()
{
}

void
WorkerResolveFetchWithResponse::OnResponseAvailable(InternalResponse* aResponse)
{
  NS_ASSERT_OWNINGTHREAD(ResolveFetchWithResponse)
  AssertIsOnMainThread();
  mInternalResponse = aResponse;

  nsRefPtr<ResolveFetchWithResponseRunnable> r =
    new ResolveFetchWithResponseRunnable(mPromiseHolder, aResponse);

  AutoSafeJSContext cx;
  if (!r->Dispatch(cx)) {
    NS_WARNING("Could not dispatch fetch resolve");
  }
}

void
WorkerResolveFetchWithResponse::OnResponseEnd()
{
  NS_ASSERT_OWNINGTHREAD(ResolveFetchWithResponse)
  AssertIsOnMainThread();
  MOZ_ASSERT(mInternalResponse);

  nsCOMPtr<nsIDOMBlob> blob = mInternalResponse->GetBody();
  nsRefPtr<DOMFile> f = static_cast<DOMFile*>(blob.get());
  MOZ_ASSERT(f);
  nsRefPtr<DOMFileImpl> fileImpl = f->Impl();
  mInternalResponse->SetBody(nullptr);

  // FIXME(nsm): The worker private could've gone by the time we do this since it will be
  // removefeatured when onresponseavailable dispatches the response.
  nsRefPtr<ResolveFetchWithBodyRunnable> r =
    new ResolveFetchWithBodyRunnable(mPromiseHolder->GetWorkerPrivate(), mInternalResponse, fileImpl);

  AutoSafeJSContext cx;
  if (!r->Dispatch(cx)) {
    NS_WARNING("Could not dispatch fetch body");
  }
}

// Empty string for no-referrer. FIXME(nsm): Does returning empty string
// actually lead to no-referrer in the base channel?
// The actual referrer policy and stripping is dealt with by HttpBaseChannel,
// this always returns the full API referrer URL of the relevant global.
nsCString
GetRequestReferrer(const InternalRequest* aRequest)
{
  nsCOMPtr<nsIGlobalObject> env = aRequest->GetClient();

  nsCString referrerSource;
  if (aRequest->ReferrerIsURL()) {
    referrerSource = aRequest->ReferrerAsURL();
  } else {
    nsCOMPtr<nsPIDOMWindow> window = do_QueryInterface(env);
    if (window) {
      nsCOMPtr<nsIDocument> doc = window->GetExtantDoc();
      if (doc) {
        nsCOMPtr<nsIURI> docURI = doc->GetDocumentURI();
        nsCString origin;
        nsresult rv = nsContentUtils::GetASCIIOrigin(docURI, origin);
        if (NS_WARN_IF(NS_FAILED(rv))) {
          return EmptyCString();
        }

        nsString referrer;
        doc->GetReferrer(referrer);
        referrerSource = NS_ConvertUTF16toUTF8(referrer);
      }
    } else {
      WorkerPrivate* worker = GetCurrentThreadWorkerPrivate();
      MOZ_ASSERT(worker);
      worker->AssertIsOnWorkerThread();
      referrerSource = worker->BaseURL();
      // FIXME(nsm): Algorithm says "If source is not a URL..." but when is it
      // not a URL?
    }
  }

  return referrerSource;
}

} // namespace dom
} // namespace mozilla
