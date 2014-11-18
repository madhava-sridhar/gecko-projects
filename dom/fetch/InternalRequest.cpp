/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "InternalRequest.h"

#include "nsIContentPolicy.h"
#include "nsIDocument.h"
#include "nsIInputStreamTee.h"
#include "nsIOutputStream.h"
#include "nsIPipe.h"

#include "mozilla/ErrorResult.h"
#include "mozilla/dom/ScriptSettings.h"
#include "mozilla/dom/workers/Workers.h"

#include "WorkerPrivate.h"

namespace mozilla {
namespace dom {

InternalRequest::InternalRequest(InternalRequest& aOther)
  : mMethod(aOther.mMethod)
  , mURL(aOther.mURL)
  , mHeaders(new InternalHeaders(*aOther.mHeaders))
  , mContext(aOther.mContext)
  , mOrigin(aOther.mOrigin)
  , mContextFrameType(aOther.mContextFrameType)
  , mReferrerType(aOther.mReferrerType)
  , mReferrerURL(aOther.mReferrerURL)
  , mMode(aOther.mMode)
  , mCredentialsMode(aOther.mCredentialsMode)
  , mResponseTainting(aOther.mResponseTainting)
  , mRedirectCount(aOther.mRedirectCount)
  , mAuthenticationFlag(aOther.mAuthenticationFlag)
  , mForceOriginHeader(aOther.mForceOriginHeader)
  , mManualRedirect(aOther.mManualRedirect)
  , mPreserveContentCodings(aOther.mPreserveContentCodings)
  , mSameOriginDataURL(aOther.mSameOriginDataURL)
  , mSandboxedStorageAreaURLs(aOther.mSandboxedStorageAreaURLs)
  , mSkipServiceWorker(aOther.mSkipServiceWorker)
  , mSynchronous(aOther.mSynchronous)
  , mUnsafeRequest(aOther.mUnsafeRequest)
  , mUseURLCredentials(aOther.mUseURLCredentials)
{
  if (!aOther.mBodyStream) {
    return;
  }

  // TODO: Replace body stream Tee once cloneable streams available (bug 1100398)
  nsCOMPtr<nsIInputStream> pipeReader;
  nsCOMPtr<nsIOutputStream> pipeWriter;
  nsresult rv = NS_NewPipe(getter_AddRefs(pipeReader),
                           getter_AddRefs(pipeWriter));
  if (NS_WARN_IF(NS_FAILED(rv))) { return; }

  nsCOMPtr<nsIInputStream> tee;
  rv = NS_NewInputStreamTee(getter_AddRefs(tee), aOther.mBodyStream,
                            pipeWriter);
  if (NS_WARN_IF(NS_FAILED(rv))) { return; }

  // The pipe reader can get stalled if the tee is not being read.  Give the
  // tee to the cloned stream as its more likely to be used first.
  aOther.mBodyStream.swap(pipeReader);
  mBodyStream.swap(tee);
}

// The global is used to extract the principal.
already_AddRefed<InternalRequest>
InternalRequest::GetRequestConstructorCopy(nsIGlobalObject* aGlobal, ErrorResult& aRv) const
{
  nsRefPtr<InternalRequest> copy = new InternalRequest();
  copy->mURL.Assign(mURL);
  copy->SetMethod(mMethod);
  copy->mHeaders = new InternalHeaders(*mHeaders);

  copy->mBodyStream = mBodyStream;
  copy->mPreserveContentCodings = true;

  if (NS_IsMainThread()) {
    nsIPrincipal* principal = aGlobal->PrincipalOrNull();
    MOZ_ASSERT(principal);
    aRv = nsContentUtils::GetASCIIOrigin(principal, copy->mOrigin);
    if (NS_WARN_IF(aRv.Failed())) {
      return nullptr;
    }
  } else {
    workers::WorkerPrivate* worker = workers::GetCurrentThreadWorkerPrivate();
    MOZ_ASSERT(worker);
    worker->AssertIsOnWorkerThread();

    workers::WorkerPrivate::LocationInfo& location = worker->GetLocationInfo();
    copy->mOrigin = NS_ConvertUTF16toUTF8(location.mOrigin);
  }

  copy->mContext = nsIContentPolicy::TYPE_FETCH;
  copy->mMode = mMode;
  copy->mCredentialsMode = mCredentialsMode;
  return copy.forget();
}

InternalRequest::~InternalRequest()
{
}

} // namespace dom
} // namespace mozilla
