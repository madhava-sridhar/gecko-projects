/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "Request.h"

#include "nsIURI.h"
#include "nsISupportsImpl.h"

#include "mozilla/dom/Promise.h"
#include "nsDOMString.h"
#include "nsPIDOMWindow.h"
#include "WorkerPrivate.h"

// #include "mozilla/dom/FetchBodyStream.h"
#include "mozilla/dom/URL.h"
#include "mozilla/dom/workers/bindings/URL.h"

namespace mozilla {
namespace dom {

NS_IMPL_CYCLE_COLLECTING_ADDREF(Request)
NS_IMPL_CYCLE_COLLECTING_RELEASE(Request)
NS_IMPL_CYCLE_COLLECTION_WRAPPERCACHE_0(Request)

NS_INTERFACE_MAP_BEGIN_CYCLE_COLLECTION(Request)
  NS_WRAPPERCACHE_INTERFACE_MAP_ENTRY
  NS_INTERFACE_MAP_ENTRY(nsISupports)
NS_INTERFACE_MAP_END

Request::Request(nsISupports* aOwner, InternalRequest* aRequest)
  : mOwner(aOwner)
  , mHeaders(new Headers(aOwner))
  , mRequest(aRequest)
{
  SetIsDOMBinding();
}

Request::~Request()
{
}

void
Request::GetHeader(const nsAString& aHeader, DOMString& aValue) const
{
  aValue.AsAString() = EmptyString();
}

already_AddRefed<Headers>
Request::Headers_() const
{
  nsRefPtr<Headers> ref = mHeaders;
  return ref.forget();
}

already_AddRefed<InternalRequest>
Request::GetInternalRequest()
{
  nsRefPtr<InternalRequest> r = mRequest;
  return r.forget();
}

/*static*/ already_AddRefed<Request>
Request::Constructor(const GlobalObject& aGlobal, const RequestOrScalarValueString& aInput,
                     const RequestInit& aInit, ErrorResult& aRv)
{
  nsRefPtr<InternalRequest> request;
  
  nsCOMPtr<nsIGlobalObject> global = do_QueryInterface(aGlobal.GetAsSupports());

  if (aInput.IsRequest()) {
    request = aInput.GetAsRequest().GetInternalRequest();
  } else {
    request = new InternalRequest(global);
  }

  request = request->GetRestrictedCopy(global);

  RequestMode fallbackMode = RequestMode::EndGuard_;
  RequestCredentials fallbackCredentials = RequestCredentials::EndGuard_;
  if (aInput.IsScalarValueString()) {
    nsString input;
    input.Assign(aInput.GetAsScalarValueString());
    
    nsString sURL;
    if (NS_IsMainThread()) {
      nsCOMPtr<nsPIDOMWindow> window = do_QueryInterface(global);
      MOZ_ASSERT(window);
      nsCOMPtr<nsIURI> docURI = window->GetDocumentURI();
      nsCString spec;
      docURI->GetSpec(spec);
      nsRefPtr<mozilla::dom::URL> url = mozilla::dom::URL::Constructor(aGlobal, input, NS_ConvertUTF8toUTF16(spec), aRv);
      if (aRv.Failed()) {
        aRv.ThrowTypeError(MSG_URL_PARSE_ERROR);
        return nullptr;
      }

      url->Stringify(sURL, aRv);
      if (aRv.Failed()) {
        aRv.ThrowTypeError(MSG_URL_PARSE_ERROR);
        return nullptr;
      }
    } else {
      workers::WorkerPrivate* worker = workers::GetCurrentThreadWorkerPrivate();
      MOZ_ASSERT(worker);
      worker->AssertIsOnWorkerThread();
      nsRefPtr<workers::URL> url = mozilla::dom::workers::URL::Constructor(aGlobal, input, NS_ConvertUTF8toUTF16(worker->BaseURL()), aRv);
      if (aRv.Failed()) {
        return nullptr;
      }

      url->Stringify(sURL, aRv);
      if (aRv.Failed()) {
        return nullptr;
      }
    }
    request->SetURL(NS_ConvertUTF16toUTF8(sURL));
    fallbackMode = RequestMode::Cors;
    fallbackCredentials = RequestCredentials::Omit;
  }

  nsCString method = aInit.mMethod.WasPassed() ? aInit.mMethod.Value() : NS_LITERAL_CSTRING("GET");

  if (method.LowerCaseEqualsLiteral("connect") ||
      method.LowerCaseEqualsLiteral("trace") ||
      method.LowerCaseEqualsLiteral("track")) {
    aRv.ThrowTypeError(MSG_INVALID_REQUEST_METHOD, method.get());
    return nullptr;
  }

  request->SetMethod(method);

  nsRefPtr<Request> domRequest = new Request(global, request);

  // FIXME(nsm): Plenty of headers and body property setting here.

  // FIXME(nsm): Headers
  // FIXME(nsm): Body setup from FetchBodyStreamInit.
  RequestMode mode = aInit.mMode.WasPassed() ? aInit.mMode.Value() : fallbackMode;
  RequestCredentials credentials = aInit.mCredentials.WasPassed() ? aInit.mCredentials.Value() : fallbackCredentials;

  if (mode != RequestMode::EndGuard_) {
    request->SetMode(mode);
  }

  if (credentials != RequestCredentials::EndGuard_) {
    request->SetCredentialsMode(credentials);
  }

  return domRequest.forget();
}

already_AddRefed<Promise>
Request::ArrayBuffer()
{
  nsCOMPtr<nsIGlobalObject> global = do_QueryInterface(GetParentObject());
  MOZ_ASSERT(global);
  ErrorResult result;
  nsRefPtr<Promise> promise = Promise::Create(global, result);
  if (result.Failed()) {
    return nullptr;
  }

  promise->MaybeReject(NS_ERROR_NOT_AVAILABLE);
  return promise.forget();
}

already_AddRefed<Promise>
Request::Blob()
{
  nsCOMPtr<nsIGlobalObject> global = do_QueryInterface(GetParentObject());
  MOZ_ASSERT(global);
  ErrorResult result;
  nsRefPtr<Promise> promise = Promise::Create(global, result);
  if (result.Failed()) {
    return nullptr;
  }

  promise->MaybeReject(NS_ERROR_NOT_AVAILABLE);
  return promise.forget();
}

already_AddRefed<Promise>
Request::FormData()
{
  nsCOMPtr<nsIGlobalObject> global = do_QueryInterface(GetParentObject());
  MOZ_ASSERT(global);
  ErrorResult result;
  nsRefPtr<Promise> promise = Promise::Create(global, result);
  if (result.Failed()) {
    return nullptr;
  }

  promise->MaybeReject(NS_ERROR_NOT_AVAILABLE);
  return promise.forget();
}

already_AddRefed<Promise>
Request::Json()
{
  nsCOMPtr<nsIGlobalObject> global = do_QueryInterface(GetParentObject());
  MOZ_ASSERT(global);
  ErrorResult result;
  nsRefPtr<Promise> promise = Promise::Create(global, result);
  if (result.Failed()) {
    return nullptr;
  }

  promise->MaybeReject(NS_ERROR_NOT_AVAILABLE);
  return promise.forget();
}

already_AddRefed<Promise>
Request::Text()
{
  nsCOMPtr<nsIGlobalObject> global = do_QueryInterface(GetParentObject());
  MOZ_ASSERT(global);
  ErrorResult result;
  nsRefPtr<Promise> promise = Promise::Create(global, result);
  if (result.Failed()) {
    return nullptr;
  }

  promise->MaybeReject(NS_ERROR_NOT_AVAILABLE);
  return promise.forget();
}

bool
Request::BodyUsed()
{
  return false;
}

} // namespace dom
} // namespace mozilla
