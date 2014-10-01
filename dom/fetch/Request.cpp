/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "Request.h"

#include "nsIUnicodeDecoder.h"
#include "nsIURI.h"

#include "nsDOMFile.h"
#include "nsDOMString.h"
#include "nsNetUtil.h"
#include "nsPIDOMWindow.h"
#include "nsStreamUtils.h"
#include "nsStringStream.h"

#include "mozilla/dom/EncodingUtils.h"
#include "mozilla/dom/Fetch.h"
#include "mozilla/dom/Promise.h"
#include "mozilla/dom/URL.h"
#include "mozilla/dom/workers/bindings/URL.h"

// dom/workers
#include "File.h"
#include "WorkerPrivate.h"

namespace mozilla {
namespace dom {

NS_IMPL_CYCLE_COLLECTING_ADDREF(Request)
NS_IMPL_CYCLE_COLLECTING_RELEASE(Request)
NS_IMPL_CYCLE_COLLECTION_WRAPPERCACHE(Request, mOwner)

NS_INTERFACE_MAP_BEGIN_CYCLE_COLLECTION(Request)
  NS_WRAPPERCACHE_INTERFACE_MAP_ENTRY
  NS_INTERFACE_MAP_ENTRY(nsISupports)
NS_INTERFACE_MAP_END

Request::Request(nsIGlobalObject* aOwner, InternalRequest* aRequest)
  : FetchBody<Request>()
  , mOwner(aOwner)
  , mRequest(aRequest)
{
  SetIsDOMBinding();
}

Request::~Request()
{
}

already_AddRefed<InternalRequest>
Request::GetInternalRequest() const
{
  nsRefPtr<InternalRequest> r = mRequest;
  return r.forget();
}

/*static*/ already_AddRefed<Request>
Request::Constructor(const GlobalObject& aGlobal,
                     const RequestOrScalarValueString& aInput,
                     const RequestInit& aInit, ErrorResult& aRv)
{
  nsRefPtr<InternalRequest> request;

  nsCOMPtr<nsIGlobalObject> global = do_QueryInterface(aGlobal.GetAsSupports());

  if (aInput.IsRequest()) {
    nsRefPtr<Request> inputReq = &aInput.GetAsRequest();
    if (inputReq->BodyUsed()) {
      aRv.ThrowTypeError(MSG_REQUEST_BODY_CONSUMED_ERROR);
      return nullptr;
    }

    inputReq->SetBodyUsed();
    request = inputReq->GetInternalRequest();
  } else {
    request = new InternalRequest(global);
  }

  request = request->GetRequestConstructorCopy(global);

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
      nsRefPtr<mozilla::dom::URL> url =
        mozilla::dom::URL::Constructor(aGlobal, input,
                                       NS_ConvertUTF8toUTF16(spec), aRv);
      if (aRv.Failed()) {
        return nullptr;
      }

      url->Stringify(sURL, aRv);
      if (aRv.Failed()) {
        return nullptr;
      }
    } else {
      workers::WorkerPrivate* worker = workers::GetCurrentThreadWorkerPrivate();
      MOZ_ASSERT(worker);
      worker->AssertIsOnWorkerThread();

      nsString baseURL = NS_ConvertUTF8toUTF16(worker->GetLocationInfo().mHref);
      nsRefPtr<workers::URL> url =
        workers::URL::Constructor(aGlobal, input, baseURL, aRv);
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

  RequestMode mode = aInit.mMode.WasPassed() ? aInit.mMode.Value() : fallbackMode;
  RequestCredentials credentials =
    aInit.mCredentials.WasPassed() ? aInit.mCredentials.Value()
                                   : fallbackCredentials;

  if (mode != RequestMode::EndGuard_) {
    request->SetMode(mode);
  }

  if (credentials != RequestCredentials::EndGuard_) {
    request->SetCredentialsMode(credentials);
  }

  if (aInit.mMethod.WasPassed()) {
    nsCString method = aInit.mMethod.Value();

    if (!method.LowerCaseEqualsLiteral("options") &&
        !method.LowerCaseEqualsLiteral("get") &&
        !method.LowerCaseEqualsLiteral("head") &&
        !method.LowerCaseEqualsLiteral("post") &&
        !method.LowerCaseEqualsLiteral("put") &&
        !method.LowerCaseEqualsLiteral("delete")) {
      NS_ConvertUTF8toUTF16 label(method);
      aRv.ThrowTypeError(MSG_INVALID_REQUEST_METHOD, &label);
      return nullptr;
    }

    ToUpperCase(method);
    request->SetMethod(method);
  }

  nsRefPtr<Request> domRequest = new Request(global, request);
  nsRefPtr<Headers> domRequestHeaders = domRequest->Headers_();

  // Step 13 - copy of domRequest headers.
  nsRefPtr<Headers> headers = new Headers(*domRequestHeaders);

  if (aInit.mHeaders.WasPassed()) {
    headers = Headers::Constructor(aGlobal, aInit.mHeaders.Value(), aRv);
    if (aRv.Failed()) {
      return nullptr;
    }
  }

  domRequestHeaders->Clear();

  if (domRequest->Mode() == RequestMode::No_cors) {
    nsCString method;
    domRequest->GetMethod(method);
    if (!method.LowerCaseEqualsLiteral("get") &&
        !method.LowerCaseEqualsLiteral("head") &&
        !method.LowerCaseEqualsLiteral("post")) {
      NS_ConvertUTF8toUTF16 label(method);
      aRv.ThrowTypeError(MSG_INVALID_REQUEST_METHOD, &label);
      return nullptr;
    }

    domRequestHeaders->SetGuard(HeadersGuardEnum::Request_no_cors, aRv);
    if (aRv.Failed()) {
      return nullptr;
    }
  }

  domRequestHeaders->Fill(*headers, aRv);
  if (aRv.Failed()) {
    return nullptr;
  }

  if (aInit.mBody.WasPassed()) {
    const OwningArrayBufferOrArrayBufferViewOrScalarValueStringOrURLSearchParams& bodyInit = aInit.mBody.Value();
    nsCOMPtr<nsIInputStream> stream;
    nsCString contentType;
    aRv = ExtractByteStreamFromBody(bodyInit,
                                    getter_AddRefs(stream), contentType);
    request->SetBody(stream);

    if (!contentType.IsVoid() &&
        !domRequestHeaders->Has(NS_LITERAL_CSTRING("Content-Type"), aRv)) {
      domRequestHeaders->Append(NS_LITERAL_CSTRING("Content-Type"),
                                contentType, aRv);
    }

    if (aRv.Failed()) {
      return nullptr;
    }
  }

  domRequest->SetMimeType(aRv);
  return domRequest.forget();
}

already_AddRefed<Request>
Request::Clone() const
{
  // FIXME(nsm): Bug 1073231. This is incorrect, but the clone method isn't
  // well defined yet.
  nsRefPtr<Request> request = new Request(mOwner,
                                          new InternalRequest(*mRequest));
  return request.forget();
}

} // namespace dom
} // namespace mozilla
