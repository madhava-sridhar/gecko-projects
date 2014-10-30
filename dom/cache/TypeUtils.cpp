/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "mozilla/dom/cache/TypeUtils.h"

#include "mozilla/unused.h"
#include "mozilla/dom/CacheBinding.h"
#include "mozilla/dom/InternalRequest.h"
#include "mozilla/dom/Request.h"
#include "mozilla/dom/Response.h"
#include "mozilla/dom/cache/PCacheTypes.h"
#include "mozilla/dom/cache/ReadStream.h"
#include "mozilla/ipc/BackgroundChild.h"
#include "mozilla/ipc/PBackgroundChild.h"
#include "mozilla/ipc/PFileDescriptorSetChild.h"
#include "mozilla/ipc/InputStreamUtils.h"
#include "nsCOMPtr.h"
#include "nsString.h"
#include "nsURLParsers.h"

// TODO: remove stream testing code
#include "nsStreamUtils.h"
#include "nsStringStream.h"

namespace {

using mozilla::ErrorResult;
using mozilla::unused;
using mozilla::void_t;
using mozilla::dom::cache::PCacheReadStream;
using mozilla::dom::cache::PCacheReadStreamOrVoid;
using mozilla::ipc::BackgroundChild;
using mozilla::ipc::FileDescriptor;
using mozilla::ipc::PFileDescriptorSetChild;
using mozilla::ipc::PBackgroundChild;

// Utility function to remove the query from a URL.  We're not using nsIURL
// or URL to do this because they require going to the main thread.
static void
GetURLWithoutQuery(const nsAString& aUrl, nsAString& aUrlWithoutQueryOut,
                   ErrorResult& aRv)
{
  NS_ConvertUTF16toUTF8 flatURL(aUrl);
  const char* url = flatURL.get();

  nsCOMPtr<nsIURLParser> urlParser = new nsStdURLParser();

  uint32_t pathPos;
  int32_t pathLen;
  nsresult rv = urlParser->ParseURL(url, flatURL.Length(),
                                    nullptr, nullptr,       // ignore scheme
                                    nullptr, nullptr,       // ignore authority
                                    &pathPos, &pathLen);
  if (NS_WARN_IF(NS_FAILED(rv))) {
    aRv.Throw(rv);
    return;
  }

  uint32_t queryPos;
  int32_t queryLen;

  rv = urlParser->ParsePath(url + pathPos, flatURL.Length() - pathPos,
                            nullptr, nullptr,               // ignore filepath
                            &queryPos, &queryLen,
                            nullptr, nullptr);              // ignore ref
  if (NS_WARN_IF(NS_FAILED(rv))) {
    aRv.Throw(rv);
    return;
  }

  if (queryLen < 0) {
    aUrlWithoutQueryOut = aUrl;
    return;
  }

  // ParsePath gives us query position relative to the start of the path
  queryPos += pathPos;

  // We want everything before and after the query
  aUrlWithoutQueryOut = Substring(aUrl, 0, queryPos - 1);
  aUrlWithoutQueryOut.Append(Substring(aUrl, queryPos + queryLen,
                                       aUrl.Length() - queryPos - queryLen));
}

void
SerializeCacheStream(nsIInputStream* aStream, PCacheReadStreamOrVoid* aStreamOut)
{
  MOZ_ASSERT(aStreamOut);
  if (!aStream) {
    *aStreamOut = void_t();
    return;
  }

  // TODO: Integrate khuey's nsFancyPipe here if aStream does not provide
  //       efficient serialization.  (Or always use pipe.)

  PCacheReadStream readStream;
  nsTArray<FileDescriptor> fds;
  SerializeInputStream(aStream, readStream.params(), fds);

  PFileDescriptorSetChild* fdSet = nullptr;
  if (!fds.IsEmpty()) {
    // We should not be serializing until we have an actor ready
    PBackgroundChild* manager = BackgroundChild::GetForCurrentThread();
    MOZ_ASSERT(manager);

    fdSet = manager->SendPFileDescriptorSetConstructor(fds[0]);
    for (uint32_t i = 1; i < fds.Length(); ++i) {
      unused << fdSet->SendAddFileDescriptor(fds[i]);
    }
  }

  if (fdSet) {
    readStream.fds() = fdSet;
  } else {
    readStream.fds() = void_t();
  }

  *aStreamOut = readStream;
}

} // anonymous namespace

namespace mozilla {
namespace dom {
namespace cache {

using mozilla::void_t;
using mozilla::ipc::BackgroundChild;
using mozilla::ipc::FileDescriptor;
using mozilla::ipc::PFileDescriptorSetChild;
using mozilla::ipc::PBackgroundChild;

// static
void
TypeUtils::ToPCacheRequest(PCacheRequest& aOut, const Request& aIn,
                           bool aReadBody, ErrorResult& aRv)
{
  aIn.GetMethod(aOut.method());
  aIn.GetUrl(aOut.url());
  GetURLWithoutQuery(aOut.url(), aOut.urlWithoutQuery(), aRv);
  if (aRv.Failed()) {
    return;
  }
  aIn.GetReferrer(aOut.referrer());
  nsRefPtr<InternalHeaders> headers = aIn.GetInternalHeaders();
  MOZ_ASSERT(headers);
  headers->GetPHeaders(aOut.headers());
  aOut.headersGuard() = headers->Guard();
  aOut.mode() = aIn.Mode();
  aOut.credentials() = aIn.Credentials();

  if (!aReadBody) {
    aOut.body() = void_t();
    return;
  }

  if (aIn.BodyUsed()) {
    aRv.ThrowTypeError(MSG_REQUEST_BODY_CONSUMED_ERROR);
    return;
  }

  nsRefPtr<InternalRequest> internalRequest = aIn.GetInternalRequest();
  MOZ_ASSERT(internalRequest);
  nsCOMPtr<nsIInputStream> stream;

  internalRequest->GetBody(getter_AddRefs(stream));
  // TODO: set Request body used

  // TODO: Provide way to send PCacheRequest without serializing body for
  //       read-only operations that do not use body.
  SerializeCacheStream(stream, &aOut.body());
}

// static
void
TypeUtils::ToPCacheRequest(const GlobalObject& aGlobal,
                           PCacheRequest& aOut,
                           const RequestOrScalarValueString& aIn,
                           bool aReadBody, ErrorResult& aRv)
{
  if (aIn.IsRequest()) {
    ToPCacheRequest(aOut, aIn.GetAsRequest(), aReadBody, aRv);
    return;
  }

  RequestInit init;
  nsRefPtr<Request> request = Request::Constructor(aGlobal, aIn, init, aRv);
  if (NS_WARN_IF(aRv.Failed())) {
    return;
  }
  ToPCacheRequest(aOut, *request, aReadBody, aRv);
}

// static
void
TypeUtils::ToPCacheRequestOrVoid(const GlobalObject& aGlobal, PCacheRequestOrVoid& aOut,
                                 const Optional<RequestOrScalarValueString>& aIn,
                                 bool aReadBody, ErrorResult& aRv)
{
  if (!aIn.WasPassed()) {
    aOut = void_t();
    return;
  }
  PCacheRequest request;
  ToPCacheRequest(aGlobal, request, aIn.Value(), aReadBody, aRv);
  if (aRv.Failed()) {
    return;
  }
  aOut = request;
}

// static
void
TypeUtils::ToPCacheRequest(const GlobalObject& aGlobal, PCacheRequest& aOut,
                           const OwningRequestOrScalarValueString& aIn,
                           bool aReadBody, ErrorResult& aRv)
{
  if (aIn.IsRequest()) {
    ToPCacheRequest(aOut, aIn.GetAsRequest(), aReadBody, aRv);
    return;
  }

  RequestOrScalarValueString input;
  RequestInit init;
  nsString str;
  str.Assign(aIn.GetAsScalarValueString());
  input.SetAsScalarValueString().Rebind(str.Data(), str.Length());

  nsRefPtr<Request> request = Request::Constructor(aGlobal, input, init, aRv);
  if (NS_WARN_IF(aRv.Failed())) {
    return;
  }
  ToPCacheRequest(aOut, *request, aReadBody, aRv);
}

// static
void
TypeUtils::ToPCacheResponse(PCacheResponse& aOut, const Response& aIn,
                            ErrorResult& aRv)
{
  aOut.type() = aIn.Type();
  aIn.GetUrl(aOut.url());
  aOut.status() = aIn.Status();
  aIn.GetStatusText(aOut.statusText());
  nsRefPtr<InternalHeaders> headers = aIn.GetInternalHeaders();
  MOZ_ASSERT(headers);
  headers->GetPHeaders(aOut.headers());
  aOut.headersGuard() = headers->Guard();

  if (aIn.BodyUsed()) {
    aRv.ThrowTypeError(MSG_REQUEST_BODY_CONSUMED_ERROR);
    return;
  }

  nsCOMPtr<nsIInputStream> stream;
  aIn.GetBody(getter_AddRefs(stream));
  // TODO: set body stream used in Response

  SerializeCacheStream(stream, &aOut.body());
}

// static
void
TypeUtils::ToPCacheQueryParams(PCacheQueryParams& aOut, const QueryParams& aIn)
{
  aOut.ignoreSearch() = aIn.mIgnoreSearch.WasPassed() &&
                        aIn.mIgnoreSearch.Value();
  aOut.ignoreMethod() = aIn.mIgnoreMethod.WasPassed() &&
                        aIn.mIgnoreMethod.Value();
  aOut.ignoreVary() = aIn.mIgnoreVary.WasPassed() &&
                      aIn.mIgnoreVary.Value();
  aOut.prefixMatch() = aIn.mPrefixMatch.WasPassed() &&
                       aIn.mPrefixMatch.Value();
  aOut.cacheNameSet() = aIn.mCacheName.WasPassed();
  if (aOut.cacheNameSet()) {
    aOut.cacheName() = aIn.mCacheName.Value();
  } else {
    aOut.cacheName() = NS_LITERAL_STRING("");
  }
}

// static
already_AddRefed<Response>
TypeUtils::ToResponse(nsIGlobalObject* aGlobal, const PCacheResponse& aIn,
                      PCacheStreamControlChild* aStreamControl)
{
  nsRefPtr<InternalResponse> ir = new InternalResponse(200, NS_LITERAL_CSTRING("OK"));

  nsCOMPtr<nsIInputStream> stream = ReadStream::Create(aStreamControl,
                                                       aIn.body());
  ir->SetBody(stream);

  nsRefPtr<Response> ref = new Response(aGlobal, ir);
  return ref.forget();
}

// static
already_AddRefed<Request>
TypeUtils::ToRequest(nsIGlobalObject* aGlobal, const PCacheRequest& aIn,
                     PCacheStreamControlChild* aStreamControl)
{
  nsRefPtr<InternalRequest> internalRequest = new InternalRequest();

  internalRequest->SetMethod(aIn.method());
  internalRequest->SetURL(NS_ConvertUTF16toUTF8(aIn.url()));
  internalRequest->SetReferrer(NS_ConvertUTF16toUTF8(aIn.referrer()));
  internalRequest->SetMode(aIn.mode());
  internalRequest->SetCredentialsMode(aIn.credentials());

  nsRefPtr<InternalHeaders> internalHeaders =
    new InternalHeaders(aIn.headers(), aIn.headersGuard());
  ErrorResult result;
  internalRequest->Headers()->SetGuard(aIn.headersGuard(), result);
  internalRequest->Headers()->Fill(*internalHeaders, result);
  MOZ_ASSERT(!result.Failed());

  nsCOMPtr<nsIInputStream> stream = ReadStream::Create(aStreamControl,
                                                       aIn.body());

  internalRequest->SetBody(stream);
  // TODO: clear request bodyRead flag
  NS_WARNING("Not clearing bodyRead flag for Request returned from Cache.");

  nsRefPtr<Request> request = new Request(aGlobal, internalRequest);
  return request.forget();
}

} // namespace cache
} // namespace dom
} // namespace mozilla
