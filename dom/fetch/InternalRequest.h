/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_dom_InternalRequest_h
#define mozilla_dom_InternalRequest_h

#include "mozilla/dom/RequestBinding.h"
#include "mozilla/dom/UnionTypes.h"

#include "nsIContentPolicy.h"
#include "nsISupportsImpl.h"

class nsIDocument;
class nsPIDOMWindow;

namespace mozilla {
namespace dom {

class FetchBodyStream;
class Request;

class InternalRequest MOZ_FINAL
{
  friend class Request;

public:
  NS_INLINE_DECL_THREADSAFE_REFCOUNTING(InternalRequest)

  enum ContextFrameType
  {
    AUXILIARY = 0,
    TOP_LEVEL,
    NESTED,
    NONE,
  };

  // Since referrer type can be none, client or a URL.
  enum ReferrerType
  {
    REFERRER_NONE = 0,
    REFERRER_CLIENT,
    REFERRER_URL,
  };

  explicit InternalRequest(nsIGlobalObject* aClient)
    : mMethod("GET")
    //, mUnsafeRequest(false)
    , mPreserveContentCodings(false)
    , mClient(aClient)
    , mSkipServiceWorker(false)
    //, mContextFrameType(NONE)
    //, mForceOriginHeader(false)
    //, mSameOriginDataURL(false)
    , mReferrerType(REFERRER_CLIENT)
    //, mAuthenticationFlag(false)
    , mSynchronous(false)
    , mMode(RequestMode::No_cors)
    , mCredentialsMode(RequestCredentials::Omit)
    //, mUseURLCredentials(false)
    //, mManualRedirect(false)
    //, mRedirectCount(0)
    //, mResponseTainting(RESPONSETAINT_BASIC)
  {
  }

  explicit InternalRequest(const InternalRequest& aRequest);

  void
  GetMethod(nsCString& aMethod) const
  {
    aMethod.Assign(mMethod);
  }

  void
  SetMethod(const nsACString& aMethod)
  {
    mMethod.Assign(aMethod);
  }

  void
  GetURL(nsCString& aURL) const
  {
    aURL.Assign(mURL);
  }

  // Should probably restrict this to friends.
  void
  SetURL(const nsACString& aURL)
  {
    mURL.Assign(aURL);
  }

  nsIGlobalObject*
  GetClient() const
  {
    return mClient;
  }

  bool
  ReferrerIsNone() const
  {
    return mReferrerType == REFERRER_NONE;
  }

  bool
  ReferrerIsURL() const
  {
    return mReferrerType == REFERRER_URL;
  }

  bool
  ReferrerIsClient() const
  {
    return mReferrerType == REFERRER_CLIENT;
  }

  nsCString
  ReferrerAsURL() const
  {
    MOZ_ASSERT(ReferrerIsURL());
    return mReferrerURL;
  }

  void
  SetReferrer(const nsACString& aReferrer)
  {
    // May be removed later.
    MOZ_ASSERT(!ReferrerIsNone());
    mReferrerType = REFERRER_URL;
    mReferrerURL.Assign(aReferrer);
  }

  void
  SetReferrer(nsIGlobalObject* aClient)
  {
    mReferrerType = REFERRER_CLIENT;
    mReferrerClient = aClient;
  }

  bool
  IsSynchronous() const
  {
    return mSynchronous;
  }

  void
  SetMode(RequestMode aMode)
  {
    mMode = aMode;
  }

  void
  SetCredentialsMode(RequestCredentials aCredentialsMode)
  {
    mCredentialsMode = aCredentialsMode;
  }

  nsContentPolicyType
  GetContext() const
  {
    return mContext;
  }

  // The global is used as the client for the new object.
  already_AddRefed<InternalRequest>
  GetRestrictedCopy(nsIGlobalObject* aGlobal) const;

private:
  ~InternalRequest();

  nsCString mMethod;
  nsCString mURL;
  // FIXME(nsm): Do we just use Headers for internal header list?

  //bool mUnsafeRequest;
  // FIXME(nsm): How do we represent body?

  bool mPreserveContentCodings;

  // FIXME(nsm): Strong ref?
  nsIGlobalObject* mClient;

  bool mSkipServiceWorker;
  nsContentPolicyType mContext;

  //ContextFrameType mContextFrameType;

  // FIXME(nsm): Should the origin be a nsIPrincipal?
  nsCString mOrigin;

  //bool mForceOriginHeader;
  //bool mSameOriginDataURL;

  ReferrerType mReferrerType;
  nsCString mReferrerURL;
  nsCOMPtr<nsIGlobalObject> mReferrerClient;

  //bool mAuthenticationFlag;
  bool mSynchronous;
  RequestMode mMode;
  RequestCredentials mCredentialsMode;
  //bool mUseURLCredentials;
  //bool mManualRedirect;
  //uint32_t mRedirectCount;
  /*enum {
    RESPONSETAINT_BASIC,
    RESPONSETAINT_CORS,
    RESPONSETAINT_OPAQUE,
  } mResponseTainting;*/
};

} // namespace dom
} // namespace mozilla

#endif // mozilla_dom_InternalRequest_h
