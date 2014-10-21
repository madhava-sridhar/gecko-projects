/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_dom_Fetch_h
#define mozilla_dom_Fetch_h

#include "nsCOMPtr.h"
#include "nsError.h"
#include "nsString.h"
#include "mozilla/ErrorResult.h"
#include "mozilla/dom/RequestBinding.h"

class nsIGlobalObject;
class nsIInputStream;
class nsIGlobalObject;

namespace mozilla {
namespace dom {

class ArrayBufferOrArrayBufferViewOrBlobOrScalarValueStringOrURLSearchParams;
class InternalRequest;
class OwningArrayBufferOrArrayBufferViewOrBlobOrScalarValueStringOrURLSearchParams;
class Promise;
class RequestOrScalarValueString;

namespace workers {
class WorkerPrivate;
} // namespace workers

already_AddRefed<Promise>
FetchRequest(nsIGlobalObject* aGlobal, const RequestOrScalarValueString& aInput,
             const RequestInit& aInit, ErrorResult& aRv);

nsresult
GetRequestReferrer(nsIGlobalObject* aGlobal, const InternalRequest* aRequest, nsCString& aReferrer);

/*
 * Creates an nsIInputStream based on the fetch specifications 'extract a byte
 * stream algorithm' - http://fetch.spec.whatwg.org/#concept-bodyinit-extract.
 * Stores content type in out param aContentType.
 */
nsresult
ExtractByteStreamFromBody(const OwningArrayBufferOrArrayBufferViewOrBlobOrScalarValueStringOrURLSearchParams& aBodyInit,
                          nsIInputStream** aStream,
                          nsCString& aContentType);

/*
 * Non-owning version.
 */
nsresult
ExtractByteStreamFromBody(const ArrayBufferOrArrayBufferViewOrBlobOrScalarValueStringOrURLSearchParams& aBodyInit,
                          nsIInputStream** aStream,
                          nsCString& aContentType);

template <class Derived>
class FetchBody {
public:
  bool
  BodyUsed() const { return mBodyUsed; }

  void
  SetBodyUsed()
  {
    mBodyUsed = true;
  }

  already_AddRefed<Promise>
  ArrayBuffer(ErrorResult& aRv)
  {
    return ConsumeBody(CONSUME_ARRAYBUFFER, aRv);
  }

  already_AddRefed<Promise>
  Blob(ErrorResult& aRv)
  {
    return ConsumeBody(CONSUME_BLOB, aRv);
  }

  already_AddRefed<Promise>
  Json(ErrorResult& aRv)
  {
    return ConsumeBody(CONSUME_JSON, aRv);
  }

  already_AddRefed<Promise>
  Text(ErrorResult& aRv)
  {
    return ConsumeBody(CONSUME_TEXT, aRv);
  }

  void
  TryFinishConsumeBody();

protected:
  FetchBody()
    : mBodyUsed(false)
  {
  }

  void
  SetMimeType(ErrorResult& aRv);

private:
  enum ConsumeType
  {
    CONSUME_ARRAYBUFFER,
    CONSUME_BLOB,
    // FormData not supported right now,
    CONSUME_JSON,
    CONSUME_TEXT,
  };

  Derived*
  DerivedClass() const
  {
    return static_cast<Derived*>(const_cast<FetchBody*>(this));
  }

  void
  FinishConsumeBody();

  already_AddRefed<Promise>
  ConsumeBody(ConsumeType aType, ErrorResult& aRv);

  bool mBodyUsed;
  nsCString mMimeType;

  // If the body stream is not yet available, the consume body algorithm will
  // initialize this promise, then wait for the stream to be available, and
  // resolve this Promise with the contents.
  //
  // The first call to ConsumeBody() sets these, subsequent calls are rejected.
  nsRefPtr<Promise> mDelayedConsumePromise;
  ConsumeType mDelayedConsumeType;
};

} // namespace dom
} // namespace mozilla

#endif // mozilla_dom_Fetch_h
