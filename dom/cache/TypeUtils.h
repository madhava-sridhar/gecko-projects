/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_dom_cache_TypesUtils_h
#define mozilla_dom_cache_TypesUtils_h

#include "mozilla/Attributes.h"

namespace mozilla {
namespace dom {

class OwningRequestOrScalarValueString;
struct QueryParams;
class InternalRequest;
class Request;
class RequestOrScalarValueString;
class Response;
template<typename T> class Optional;

namespace cache {

class PCacheQueryParams;
class PCacheRequest;
class PCacheRequestOrVoid;
class PCacheResponse;

class TypeUtils
{
public:
  static void
  ToPCacheRequest(PCacheRequest& aOut, const Request& aIn);

  static void
  ToPCacheRequest(PCacheRequest& aOut, const RequestOrScalarValueString& aIn);

  static void
  ToPCacheRequestOrVoid(PCacheRequestOrVoid& aOut,
                        const Optional<RequestOrScalarValueString>& aIn);

  static void
  ToPCacheRequest(PCacheRequest& aOut,
                  const OwningRequestOrScalarValueString& aIn);

  static void
  ToPCacheResponse(PCacheResponse& aOut, const Response& aIn);

  static void
  ToPCacheQueryParams(PCacheQueryParams& aOut, const QueryParams& aIn);

  static void
  ToResponse(Response& aOut, const PCacheResponse& aIn);

  static void
  ToInternalRequest(InternalRequest& aOut, const PCacheRequest& aIn);

private:
  TypeUtils() MOZ_DELETE;
  ~TypeUtils() MOZ_DELETE;
};

} // namespace cache
} // namespace dom
} // namespace mozilla

#endif // mozilla_dom_cache_TypesUtils_h
