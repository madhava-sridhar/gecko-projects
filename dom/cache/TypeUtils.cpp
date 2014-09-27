/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "mozilla/dom/cache/TypeUtils.h"

#include "mozilla/dom/CacheBinding.h"
#include "mozilla/dom/InternalRequest.h"
#include "mozilla/dom/Request.h"
#include "mozilla/dom/Response.h"
#include "mozilla/dom/cache/PCacheTypes.h"
#include "nsCOMPtr.h"
#include "nsString.h"
#include "nsURLParsers.h"

namespace {

// Utility function to remove the query from a URL.  We're not using nsIURL
// or URL to do this because they require going to the main thread.
static nsresult
GetURLWithoutQuery(const nsAString& aUrl, nsAString& aUrlWithoutQueryOut)
{
  NS_ConvertUTF16toUTF8 flatURL(aUrl);
  const char* url = flatURL.get();

  nsCOMPtr<nsIURLParser> urlParser = new nsStdURLParser();
  NS_ENSURE_TRUE(urlParser, NS_ERROR_OUT_OF_MEMORY);

  uint32_t pathPos;
  int32_t pathLen;

  nsresult rv = urlParser->ParseURL(url, flatURL.Length(),
                                    nullptr, nullptr,       // ignore scheme
                                    nullptr, nullptr,       // ignore authority
                                    &pathPos, &pathLen);
  NS_ENSURE_SUCCESS(rv, rv);

  uint32_t queryPos;
  int32_t queryLen;

  rv = urlParser->ParsePath(url + pathPos, flatURL.Length() - pathPos,
                            nullptr, nullptr,               // ignore filepath
                            &queryPos, &queryLen,
                            nullptr, nullptr);              // ignore ref
  NS_ENSURE_SUCCESS(rv, rv);

  // ParsePath gives us query position relative to the start of the path
  queryPos += pathPos;

  // We want everything before and after the query
  aUrlWithoutQueryOut = Substring(aUrl, 0, queryPos);
  aUrlWithoutQueryOut.Append(Substring(aUrl, queryPos + queryLen,
                                       aUrl.Length() - queryPos - queryLen));

  return NS_OK;
}

} // anonymous namespace

namespace mozilla {
namespace dom {
namespace cache {

using mozilla::void_t;

// static
void
TypeUtils::ToPCacheRequest(PCacheRequest& aOut, const Request& aIn)
{
  aIn.GetMethod(aOut.method());
  aIn.GetUrl(aOut.url());
  if(NS_WARN_IF(NS_FAILED(GetURLWithoutQuery(aOut.url(),
                                              aOut.urlWithoutQuery())))) {
    // Fallback to just not providing ignoreSearch support
    // TODO: Should we error out here instead?
    aIn.GetUrl(aOut.urlWithoutQuery());
  }
  aIn.GetReferrer(aOut.referrer());
  nsRefPtr<Headers> headers = aIn.Headers_();
  MOZ_ASSERT(headers);
  headers->GetPHeaders(aOut.headers());
  aOut.headersGuard() = headers->Guard();
  aOut.mode() = aIn.Mode();
  aOut.credentials() = aIn.Credentials();
}

// static
void
TypeUtils::ToPCacheRequest(PCacheRequest& aOut,
                           const RequestOrScalarValueString& aIn)
{
  nsRefPtr<Request> request;
  if (aIn.IsRequest()) {
    request = &aIn.GetAsRequest();
  } else {
    MOZ_ASSERT(aIn.IsScalarValueString());
    // TODO: see nsIStandardURL.init() if Request does not provide something...
    MOZ_CRASH("implement me");
  }
  ToPCacheRequest(aOut, *request);
}

// static
void
TypeUtils::ToPCacheRequestOrVoid(PCacheRequestOrVoid& aOut,
                                 const Optional<RequestOrScalarValueString>& aIn)
{
  if (!aIn.WasPassed()) {
    aOut = void_t();
    return;
  }
  PCacheRequest request;
  ToPCacheRequest(request, aIn.Value());
  aOut = request;
}

// static
void
TypeUtils::ToPCacheRequest(PCacheRequest& aOut,
                           const OwningRequestOrScalarValueString& aIn)
{
  nsRefPtr<Request> request;
  if (aIn.IsRequest()) {
    request = &static_cast<Request&>(aIn.GetAsRequest());
  } else {
    MOZ_ASSERT(aIn.IsScalarValueString());
    MOZ_CRASH("implement me");
  }
  ToPCacheRequest(aOut, *request);
}

// static
void
TypeUtils::ToPCacheResponse(PCacheResponse& aOut, const Response& aIn)
{
  aOut.type() = aIn.Type();
  aIn.GetUrl(aOut.url());
  aOut.status() = aIn.Status();
  aIn.GetStatusText(aOut.statusText());
  nsRefPtr<Headers> headers = aIn.Headers_();
  MOZ_ASSERT(headers);
  headers->GetPHeaders(aOut.headers());
  aOut.headersGuard() = headers->Guard();
}

// static
void
TypeUtils:: ToPCacheQueryParams(PCacheQueryParams& aOut, const QueryParams& aIn)
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
void
TypeUtils::ToResponse(Response& aOut, const PCacheResponse& aIn)
{
  // TODO: implement once real Request/Response are available
  NS_WARNING("Not filling in contents of Response returned from Cache.");
}

// static
void
TypeUtils::ToInternalRequest(InternalRequest& aOut, const PCacheRequest& aIn)
{
  aOut.SetMethod(aIn.method());
  aOut.SetURL(NS_ConvertUTF16toUTF8(aIn.url()));
  aOut.SetReferrer(NS_ConvertUTF16toUTF8(aIn.referrer()));
  aOut.SetMode(aIn.mode());
  aOut.SetCredentialsMode(aIn.credentials());
  nsRefPtr<Headers> headers = new Headers(aOut.GetClient(), aIn.headers(),
                                          aIn.headersGuard());
  aOut.SetHeaders(headers);
}

} // namespace cache
} // namespace dom
} // namespace mozilla
