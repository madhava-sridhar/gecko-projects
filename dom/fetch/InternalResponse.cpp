/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "InternalResponse.h"

#include "nsIDOMFile.h"
#include "nsIInputStreamTee.h"
#include "nsIOutputStream.h"
#include "nsIPipe.h"

#include "mozilla/dom/InternalHeaders.h"

namespace mozilla {
namespace dom {

InternalResponse::InternalResponse(uint16_t aStatus, const nsACString& aStatusText)
  : mType(ResponseType::Default)
  , mStatus(aStatus)
  , mStatusText(aStatusText)
  , mHeaders(new InternalHeaders(HeadersGuardEnum::Response))
{
}

// Headers are not copied since BasicResponse and CORSResponse both need custom
// header handling.
InternalResponse::InternalResponse(InternalResponse& aOther)
  : mType(aOther.mType)
  , mTerminationReason(aOther.mTerminationReason)
  , mURL(aOther.mURL)
  , mStatus(aOther.mStatus)
  , mStatusText(aOther.mStatusText)
  , mContentType(aOther.mContentType)
{
  if (!aOther.mBody) {
    return;
  }

  // TODO: Replace body stream Tee once cloneable streams available (bug 1100398)
  nsCOMPtr<nsIInputStream> pipeReader;
  nsCOMPtr<nsIOutputStream> pipeWriter;
  nsresult rv = NS_NewPipe(getter_AddRefs(pipeReader),
                           getter_AddRefs(pipeWriter));
  if (NS_WARN_IF(NS_FAILED(rv))) { return; }

  nsCOMPtr<nsIInputStream> tee;
  rv = NS_NewInputStreamTee(getter_AddRefs(tee), aOther.mBody,
                            pipeWriter);
  if (NS_WARN_IF(NS_FAILED(rv))) { return; }

  // The pipe reader can get stalled if the tee is not being read.  Give the
  // tee to the cloned stream as its more likely to be used first.
  aOther.mBody.swap(pipeReader);
  mBody.swap(tee);
}

already_AddRefed<InternalResponse>
InternalResponse::Clone()
{
  nsRefPtr<InternalResponse> ir = new InternalResponse(*this);
  ir->mHeaders = new InternalHeaders(*mHeaders);
  return ir.forget();
}

// static
already_AddRefed<InternalResponse>
InternalResponse::BasicResponse(InternalResponse* aInner)
{
  MOZ_ASSERT(aInner);
  nsRefPtr<InternalResponse> basic = new InternalResponse(*aInner);
  basic->mType = ResponseType::Basic;
  basic->mHeaders = InternalHeaders::BasicHeaders(aInner->mHeaders);
  return basic.forget();
}

// static
already_AddRefed<InternalResponse>
InternalResponse::CORSResponse(InternalResponse* aInner)
{
  MOZ_ASSERT(aInner);
  nsRefPtr<InternalResponse> cors = new InternalResponse(*aInner);
  cors->mType = ResponseType::Cors;
  cors->mHeaders = InternalHeaders::CORSHeaders(aInner->mHeaders);
  return cors.forget();
}

} // namespace dom
} // namespace mozilla
