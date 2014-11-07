/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_dom_cache_TypesUtils_h
#define mozilla_dom_cache_TypesUtils_h

#include "mozilla/Attributes.h"
#include "mozilla/dom/BindingUtils.h"
#include "nsCOMPtr.h"
#include "nsError.h"

class nsIGlobalObject;
class nsIInputStream;

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
class PCacheReadStreamOrVoid;
class PCacheRequest;
class PCacheRequestOrVoid;
class PCacheResponse;
class PCacheStreamControlChild;

class TypeUtils
{
protected:
  virtual ~TypeUtils() { }
  virtual nsIGlobalObject* GetGlobalObject() const=0;
#ifdef DEBUG
  virtual void AssertOwningThread() const=0;
#else
  inline void AssertOwningThread() { }
#endif

  void
  ToPCacheRequest(PCacheRequest& aOut,
                  const RequestOrScalarValueString& aIn, bool aReadBody,
                  ErrorResult& aRv);

  void
  ToPCacheRequest(PCacheRequest& aOut,
                  const OwningRequestOrScalarValueString& aIn,
                  bool aReadBody, ErrorResult& aRv);

  void
  ToPCacheRequestOrVoid(PCacheRequestOrVoid& aOut,
                        const Optional<RequestOrScalarValueString>& aIn,
                        bool aReadBody, ErrorResult& aRv);

  void
  ToPCacheRequest(PCacheRequest& aOut, Request& aIn, bool aReadBody,
                  ErrorResult& aRv);

  void
  ToPCacheResponse(PCacheResponse& aOut, Response& aIn, ErrorResult& aRv);

  void
  ToPCacheQueryParams(PCacheQueryParams& aOut, const QueryParams& aIn);

  already_AddRefed<Response>
  ToResponse(const PCacheResponse& aIn,
             PCacheStreamControlChild* aStreamControl);

  already_AddRefed<Request>
  ToRequest(const PCacheRequest& aIn,
            PCacheStreamControlChild* aStreamControl);

private:
  void
  ToPCacheRequest(const GlobalObject& aGlobal, PCacheRequest& aOut,
                  const RequestOrScalarValueString& aIn, bool aReadBody,
                  ErrorResult& aRv);

  void
  ToPCacheRequestOrVoid(const GlobalObject& aGlobal,
                        PCacheRequestOrVoid& aOut,
                        const Optional<RequestOrScalarValueString>& aIn,
                        bool aReadBody, ErrorResult& aRv);

  void
  ToPCacheRequest(const GlobalObject& aGlobal, PCacheRequest& aOut,
                  const OwningRequestOrScalarValueString& aIn,
                  bool aReadBody, ErrorResult& aRv);

  void
  SerializeCacheStream(nsIInputStream* aStream, PCacheReadStreamOrVoid* aStreamOut,
                       ErrorResult& aRv);

  nsIThread* GetStreamThread();

  nsCOMPtr<nsIThread> mStreamThread;
};

} // namespace cache
} // namespace dom
} // namespace mozilla

#endif // mozilla_dom_cache_TypesUtils_h
