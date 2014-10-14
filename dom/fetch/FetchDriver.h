/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_dom_FetchDriver_h
#define mozilla_dom_FetchDriver_h

#include "nsAutoPtr.h"
#include "nsIStreamListener.h"
#include "nsRefPtr.h"

class nsIAsyncOutputStream;
class nsIDocument;
class nsIPrincipal;
class nsPIDOMWindow;

namespace mozilla {
namespace dom {

class BlobSet;
class InternalRequest;
class InternalResponse;

class FetchDriverObserver
{
public:
  NS_INLINE_DECL_THREADSAFE_REFCOUNTING(FetchDriverObserver);
  virtual void OnResponseAvailable(InternalResponse* aResponse) = 0;
  // This is triggered after the response's body has been set to a valid
  // stream. ConsumeBody may proceed to read the stream.
  virtual void OnResponseEnd() = 0;

protected:
  virtual ~FetchDriverObserver()
  { };
};

class FetchDriver MOZ_FINAL : public nsIStreamListener
{
public:
  NS_DECL_ISUPPORTS
  NS_DECL_NSIREQUESTOBSERVER
  NS_DECL_NSISTREAMLISTENER

  explicit FetchDriver(InternalRequest* aRequest, nsIPrincipal* aPrincipal, nsIDocument* aDoc = nullptr);
  NS_IMETHOD Fetch(FetchDriverObserver* aObserver);

private:
  nsCOMPtr<nsIPrincipal> mPrincipal;
  nsCOMPtr<nsIDocument> mDocument;
  nsRefPtr<InternalRequest> mRequest;
  nsRefPtr<InternalResponse> mResponse;
  nsCOMPtr<nsIAsyncOutputStream> mPipeOutputStream;
  nsRefPtr<FetchDriverObserver> mObserver;
  uint32_t mFetchRecursionCount;

  FetchDriver() MOZ_DELETE;
  FetchDriver(const FetchDriver&) MOZ_DELETE;
  FetchDriver& operator=(const FetchDriver&) MOZ_DELETE;
  ~FetchDriver();

  nsresult Fetch(bool aCORSFlag);
  nsresult ContinueFetch(bool aCORSFlag);
  nsresult BasicFetch();
  nsresult HttpFetch(bool aCORSFlag = false, bool aPreflightCORSFlag = false, bool aAuthenticationFlag = false);
  nsresult ContinueHttpFetchAfterServiceWorker();
  nsresult ContinueHttpFetchAfterCORSPreflight();
  nsresult HttpNetworkFetch();
  nsresult ContinueHttpFetchAfterNetworkFetch();
  // Returns the filtered response sent to the observer.
  already_AddRefed<InternalResponse>
  BeginAndGetFilteredResponse(InternalResponse* aResponse);
  // Utility since not all cases need to do any post processing of the filtered
  // response.
  void BeginResponse(InternalResponse* aResponse);
  nsresult FailWithNetworkError();
  nsresult SucceedWithResponse();
};

} // namespace dom
} // namespace mozilla

#endif // mozilla_dom_FetchDriver_h
