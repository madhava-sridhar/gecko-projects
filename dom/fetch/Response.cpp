/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "Response.h"

#include "nsISupportsImpl.h"
#include "nsIURI.h"
#include "nsPIDOMWindow.h"

#include "InternalResponse.h"
#include "nsDOMString.h"

#include "File.h" // workers/File.h

using namespace mozilla::dom;

NS_IMPL_CYCLE_COLLECTING_ADDREF(Response)
NS_IMPL_CYCLE_COLLECTING_RELEASE(Response)
NS_IMPL_CYCLE_COLLECTION_WRAPPERCACHE_0(Response)

NS_INTERFACE_MAP_BEGIN_CYCLE_COLLECTION(Response)
  NS_WRAPPERCACHE_INTERFACE_MAP_ENTRY
  NS_INTERFACE_MAP_ENTRY(nsISupports)
NS_INTERFACE_MAP_END

Response::Response(nsISupports* aOwner)
  : mOwner(aOwner)
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
Response::Redirect(const GlobalObject& aGlobal, const nsAString& aUrl,
                   uint16_t aStatus)
{
  return nullptr;
}

/*static*/ already_AddRefed<Response>
Response::Constructor(const GlobalObject& global,
                      const Optional<ArrayBufferOrArrayBufferViewOrBlobOrFormDataOrScalarValueStringOrURLSearchParams>& aBody,
                      const ResponseInit& aInit, ErrorResult& rv)
{
  nsRefPtr<Response> response = new Response(global.GetAsSupports());
  return response.forget();
}

already_AddRefed<Promise>
Response::ArrayBuffer()
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
Response::Blob()
{
  nsCOMPtr<nsIGlobalObject> global = do_QueryInterface(GetParentObject());
  MOZ_ASSERT(global);
  ErrorResult result;
  nsRefPtr<Promise> promise = Promise::Create(global, result);
  if (result.Failed()) {
    return nullptr;
  }

  nsCOMPtr<nsIDOMBlob> blob = mInternalResponse->GetBody();
  // FIXME(nsm): Not ready to be async yet.
  MOZ_ASSERT(blob);
  ThreadsafeAutoJSContext cx;
  JS::Rooted<JS::Value> val(cx);
  nsresult rv;
  if (NS_IsMainThread()) {
    rv = nsContentUtils::WrapNative(cx, blob, &val);
  } else {
    val.setObject(*file::CreateBlob(cx, blob));
    rv = NS_OK;
  }

  if (NS_FAILED(rv)) {
    promise->MaybeReject(rv);
  } else {
    promise->MaybeResolve(cx, val);
  }
  return promise.forget();
}

already_AddRefed<Promise>
Response::FormData()
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
Response::Json()
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
Response::Text()
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
Response::BodyUsed()
{
  return false;
}
