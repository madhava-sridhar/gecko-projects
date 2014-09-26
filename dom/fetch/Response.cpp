/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "Response.h"
#include "mozilla/dom/Headers.h"
#include "mozilla/dom/Promise.h"
#include "nsIDOMFile.h"
#include "nsISupportsImpl.h"
#include "nsIURI.h"
#include "nsPIDOMWindow.h"

#include "InternalResponse.h"
#include "nsDOMString.h"

#include "File.h" // workers/File.h

namespace mozilla {
namespace dom {

using mozilla::ErrorResult;

NS_IMPL_CYCLE_COLLECTING_ADDREF(Response)
NS_IMPL_CYCLE_COLLECTING_RELEASE(Response)
NS_IMPL_CYCLE_COLLECTION_WRAPPERCACHE_0(Response)

NS_INTERFACE_MAP_BEGIN_CYCLE_COLLECTION(Response)
  NS_WRAPPERCACHE_INTERFACE_MAP_ENTRY
  NS_INTERFACE_MAP_ENTRY(nsISupports)
NS_INTERFACE_MAP_END

Response::Response(nsISupports* aOwner)
  : mOwner(aOwner)
  , mInternalResponse(new InternalResponse(200, NS_LITERAL_CSTRING("OK")))
{
  SetIsDOMBinding();
}

Response::Response(nsIGlobalObject* aGlobal, InternalResponse* aInternalResponse)
  : mOwner(aGlobal)
  , mInternalResponse(aInternalResponse)
{
  SetIsDOMBinding();
  // nsCOMPtr<nsIDOMBlob> body = aInternalResponse->GetBody();
  // SetBody(body);
}

Response::~Response()
{
}

already_AddRefed<Headers>
Response::Headers_() const
{
  return mInternalResponse->Headers_();
}

/* static */ already_AddRefed<Response>
Response::Error(const GlobalObject& aGlobal)
{
  ErrorResult result;
  ResponseInit init;
  init.mStatus = 0;
  Optional<ArrayBufferOrArrayBufferViewOrScalarValueStringOrURLSearchParams> body;
  nsRefPtr<Response> r = Response::Constructor(aGlobal, body, init, result);
  return r.forget();
}

/* static */ already_AddRefed<Response>
Response::Redirect(const GlobalObject& aGlobal, const nsAString& aUrl,
                   uint16_t aStatus)
{
  return nullptr;
}

/*static*/ already_AddRefed<Response>
Response::Constructor(const GlobalObject& aGlobal,
                      const Optional<ArrayBufferOrArrayBufferViewOrScalarValueStringOrURLSearchParams>& aBody,
                      const ResponseInit& aInit, ErrorResult& rv)
{
  nsCOMPtr<nsIGlobalObject> global = do_QueryInterface(aGlobal.GetAsSupports());
  nsRefPtr<InternalResponse> internal = new InternalResponse(aInit.mStatus,
    aInit.mStatusText.WasPassed() ? aInit.mStatusText.Value() : NS_LITERAL_CSTRING("OK"));
  nsRefPtr<Response> response = new Response(global, internal);
  return response.forget();
}

already_AddRefed<Response>
Response::Clone()
{
  nsRefPtr<Response> response = new Response(mOwner);
  return response.forget();
}

already_AddRefed<Promise>
Response::ArrayBuffer(ErrorResult& result)
{
  nsCOMPtr<nsIGlobalObject> global = do_QueryInterface(GetParentObject());
  MOZ_ASSERT(global);
  nsRefPtr<Promise> promise = Promise::Create(global, result);
  if (result.Failed()) {
    return nullptr;
  }

  promise->MaybeReject(NS_ERROR_NOT_AVAILABLE);
  return promise.forget();
}

already_AddRefed<Promise>
Response::Blob(ErrorResult& result)
{
  nsCOMPtr<nsIGlobalObject> global = do_QueryInterface(GetParentObject());
  MOZ_ASSERT(global);
  nsRefPtr<Promise> promise = Promise::Create(global, result);
  if (result.Failed()) {
    return nullptr;
  }

  nsCOMPtr<nsIDOMBlob> blob = mInternalResponse->GetBody();
  // FIXME(nsm): Not ready to be async yet.
  MOZ_ASSERT(blob);
  ThreadsafeAutoJSContext cx;
  JS::Rooted<JS::Value> val(cx);
  if (NS_IsMainThread()) {
    result = nsContentUtils::WrapNative(cx, blob, &val);
  } else {
    val.setObject(*workers::file::CreateBlob(cx, blob));
    result = NS_OK;
  }

  if (result.Failed()) {
    return nullptr;
  }

  promise->MaybeResolve(cx, val);
  return promise.forget();
}

already_AddRefed<Promise>
Response::Json(ErrorResult& result)
{
  nsCOMPtr<nsIGlobalObject> global = do_QueryInterface(GetParentObject());
  MOZ_ASSERT(global);
  nsRefPtr<Promise> promise = Promise::Create(global, result);
  if (result.Failed()) {
    return nullptr;
  }

  promise->MaybeReject(NS_ERROR_NOT_AVAILABLE);
  return promise.forget();
}

already_AddRefed<Promise>
Response::Text(ErrorResult& result)
{
  nsCOMPtr<nsIGlobalObject> global = do_QueryInterface(GetParentObject());
  MOZ_ASSERT(global);
  nsRefPtr<Promise> promise = Promise::Create(global, result);
  if (result.Failed()) {
    return nullptr;
  }

  promise->MaybeReject(NS_ERROR_NOT_AVAILABLE);
  return promise.forget();
}

bool
Response::BodyUsed()
{
  return false;
}

} // namespace dom
} // namespace mozilla
