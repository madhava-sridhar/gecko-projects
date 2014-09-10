/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "InternalRequest.h"

#include "nsIDocument.h"

#include "mozilla/dom/ScriptSettings.h"
#include "mozilla/dom/workers/Workers.h"

namespace mozilla {
namespace dom {

InternalRequest::InternalRequest(const InternalRequest& aRequest)
  : mMethod(aRequest.mMethod),
    mURL(aRequest.mURL),
    mPreserveContentCodings(aRequest.mPreserveContentCodings),
    mClient(aRequest.mClient),
    mSkipServiceWorker(aRequest.mSkipServiceWorker),
    mContext(aRequest.mContext),
    mOrigin(aRequest.mOrigin),
    mReferrerType(aRequest.mReferrerType),
    mReferrerURL(aRequest.mReferrerURL),
    //mReferrerClient(aRequest.mReferrerClient),
    mSynchronous(aRequest.mSynchronous),
    mMode(aRequest.mMode),
    mCredentialsMode(aRequest.mCredentialsMode)
{
  MOZ_ASSERT(ReferrerIsNone() || ReferrerIsURL());
}

already_AddRefed<InternalRequest>
InternalRequest::GetRestrictedCopy(nsIGlobalObject* aGlobal) const
{
  nsRefPtr<InternalRequest> copy = new InternalRequest(aGlobal);
  copy->mURL.Assign(mURL);
  copy->SetMethod(mMethod);
  // FIXME(nsm): Headers list.
  // FIXME(nsm): Tee body.
  copy->mPreserveContentCodings = true;
  
  if (NS_IsMainThread()) {
    nsIPrincipal* principal = aGlobal->PrincipalOrNull();
    MOZ_ASSERT(principal);
    nsContentUtils::GetASCIIOrigin(principal, copy->mOrigin);
  } else {
    workers::WorkerPrivate* worker = workers::GetCurrentThreadWorkerPrivate();
    MOZ_ASSERT(worker);
    worker->AssertIsOnWorkerThread();

    // FIXME(nsm): Set origin.
  }

  copy->SetReferrer(mClient);
  // FIXME(nsm): Set context;

  copy->mMode = mMode;
  copy->mCredentialsMode = mCredentialsMode;
  return copy.forget();
}

InternalRequest::~InternalRequest()
{
}

} // namespace dom
} // namespace mozilla
