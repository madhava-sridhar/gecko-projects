/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_dom_InternalResponse_h
#define mozilla_dom_InternalResponse_h

#include "nsISupportsImpl.h"

#include "mozilla/dom/Headers.h"
#include "mozilla/dom/ResponseBinding.h"
#include "nsIDOMFile.h"

namespace mozilla {
namespace dom {

class InternalResponse MOZ_FINAL
{
  friend class FetchDriver;

  ~InternalResponse()
  { }

  ResponseType mType;
  nsCString mTerminationReason;
  nsCString mURL;
  const uint32_t mStatus;
  const nsCString mStatusText;
  nsRefPtr<Headers> mHeaders;
  nsCOMPtr<nsIDOMBlob> mBody;
  nsCString mContentType;
  bool mError;

public:
  NS_INLINE_DECL_THREADSAFE_REFCOUNTING(InternalResponse)

  InternalResponse(uint32_t aStatus, const nsACString& aStatusText)
    : mStatus(aStatus)
    , mStatusText(aStatusText)
    , mHeaders(new Headers(nullptr))
    , mError(false)
  { }

  // Does not copy the body over!
  explicit InternalResponse(const InternalResponse& aOther)
    : mStatus(aOther.mStatus)
    , mStatusText(aOther.mStatusText)
    , mError(aOther.mError)
    // FIXME(nsm): Copy the rest of the stuff.
  {
  }

  static already_AddRefed<InternalResponse>
  NetworkError()
  {
    nsRefPtr<InternalResponse> response = new InternalResponse(0, NS_LITERAL_CSTRING(""));
    response->mError = true;
    return response.forget();
  }

  ResponseType
  Type() const
  {
    return mType;
  }

  bool
  IsError() const
  {
    return mError;
  }

  nsCString&
  GetUrl()
  {
    return mURL;
  }

  uint32_t
  GetStatus() const
  {
    return mStatus;
  }

  const nsCString&
  GetStatusText() const
  {
    return mStatusText;
  }

  already_AddRefed<Headers>
  Headers_() const
  {
    nsRefPtr<Headers> h = mHeaders;
    return h.forget();
  }

  already_AddRefed<nsIDOMBlob>
  GetBody();

  void
  SetBody(nsIDOMBlob* aBody)
  {
    mBody = aBody;
  }
};

} // namespace dom
} // namespace mozilla

#endif // mozilla_dom_InternalResponse_h
