/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "Fetch.h"

#include "nsIDocument.h"
#include "nsIGlobalObject.h"
#include "nsIStringStream.h"
#include "nsIUnicodeDecoder.h"
#include "nsIUnicodeEncoder.h"

#include "nsDOMString.h"
#include "nsNetUtil.h"
#include "nsStreamUtils.h"
#include "nsStringStream.h"

#include "mozilla/ArrayBufferBuilder.h"
#include "mozilla/ErrorResult.h"
#include "mozilla/dom/BlobSet.h"
#include "mozilla/dom/EncodingUtils.h"
#include "mozilla/dom/FetchDriver.h"
#include "mozilla/dom/File.h"
#include "mozilla/dom/Headers.h"
#include "mozilla/dom/Promise.h"
#include "mozilla/dom/PromiseWorkerProxy.h"
#include "mozilla/dom/Request.h"
#include "mozilla/dom/Response.h"
#include "mozilla/dom/ScriptSettings.h"
#include "mozilla/dom/URLSearchParams.h"
#include "mozilla/dom/WorkerScope.h"
#include "mozilla/dom/workers/Workers.h"

#include "InternalResponse.h"
// dom/workers
#include "WorkerPrivate.h"
#include "WorkerRunnable.h"

namespace mozilla {
namespace dom {

using namespace workers;

class WorkerFetchResolver MOZ_FINAL : public FetchDriverObserver,
                                      public WorkerFeature
{
  friend class MainThreadFetchRunnable;
  friend class WorkerFetchResponseEndRunnable;
  friend class WorkerFetchResponseRunnable;

  workers::WorkerPrivate* mWorkerPrivate;

  Mutex mCleanUpLock;
  bool mCleanedUp;
  // The following are initialized and used exclusively on the worker thread.
  nsRefPtr<Promise> mFetchPromise;
  nsRefPtr<Response> mResponse;
public:

  WorkerFetchResolver(workers::WorkerPrivate* aWorkerPrivate, Promise* aPromise)
    : mWorkerPrivate(aWorkerPrivate)
    , mCleanUpLock("WorkerFetchResolver")
    , mCleanedUp(false)
    , mFetchPromise(aPromise)
  {
  }

  void
  OnResponseAvailable(InternalResponse* aResponse) MOZ_OVERRIDE;

  void
  OnResponseEnd() MOZ_OVERRIDE;

  bool
  Notify(JSContext* aCx, Status aStatus) MOZ_OVERRIDE
  {
    if (aStatus > Running) {
      CleanUp(aCx);
    }
    return true;
  }

  void
  CleanUp(JSContext* aCx)
  {
    MutexAutoLock lock(mCleanUpLock);

    if (mCleanedUp) {
      return;
    }

    MOZ_ASSERT(mWorkerPrivate);
    mWorkerPrivate->AssertIsOnWorkerThread();
    MOZ_ASSERT(mWorkerPrivate->GetJSContext() == aCx);

    mWorkerPrivate->RemoveFeature(aCx, this);
    CleanUpUnchecked();
  }

  void
  CleanUpUnchecked()
  {
    if (mFetchPromise) {
      mFetchPromise->MaybeReject(NS_ERROR_DOM_ABORT_ERR);
      mFetchPromise = nullptr;
    }
    mCleanedUp = true;
  }

  workers::WorkerPrivate*
  GetWorkerPrivate() const
  {
    // It's ok to race on |mCleanedUp|, because it will never cause us to fire
    // the assertion when we should not.
    MOZ_ASSERT(!mCleanedUp);
    return mWorkerPrivate;
  }

private:
  ~WorkerFetchResolver()
  {
    MOZ_ASSERT(mCleanedUp);
    MOZ_ASSERT(!mFetchPromise);
  }
};

class ReleaseWorkerFetchResolverRunnable MOZ_FINAL : public MainThreadWorkerControlRunnable
{
  nsRefPtr<WorkerFetchResolver> mResolver;
public:
  ReleaseWorkerFetchResolverRunnable(WorkerFetchResolver *aResolver)
    : MainThreadWorkerControlRunnable(aResolver->GetWorkerPrivate())
    , mResolver(aResolver)
  { }

  bool
  WorkerRun(JSContext* aCx, WorkerPrivate* aWorkerPrivate) MOZ_OVERRIDE
  {
    nsRefPtr<WorkerFetchResolver> release = mResolver.forget();
    release->CleanUp(aCx);
    return true;
  }
};

class MainThreadFetchResolver MOZ_FINAL : public FetchDriverObserver
{
  nsRefPtr<Promise> mPromise;
  nsRefPtr<Response> mResponse;

  NS_DECL_OWNINGTHREAD
public:
  explicit MainThreadFetchResolver(Promise* aPromise);

  void
  OnResponseAvailable(InternalResponse* aResponse) MOZ_OVERRIDE;

  void
  OnResponseEnd() MOZ_OVERRIDE;

private:
  ~MainThreadFetchResolver();
};

class MainThreadFetchRunnable : public nsRunnable
{
  nsRefPtr<WorkerFetchResolver> mResolver;
  nsRefPtr<InternalRequest> mRequest;

public:
  MainThreadFetchRunnable(WorkerPrivate* aWorkerPrivate,
                          Promise* aPromise,
                          InternalRequest* aRequest)
    : mResolver(new WorkerFetchResolver(aWorkerPrivate, aPromise))
    , mRequest(aRequest)
  {
    MOZ_ASSERT(aWorkerPrivate);
    aWorkerPrivate->AssertIsOnWorkerThread();
    if (!aWorkerPrivate->AddFeature(aWorkerPrivate->GetJSContext(), mResolver)) {
      NS_WARNING("Could not add WorkerFetchResolver feature to worker");
      mResolver->CleanUpUnchecked();
      mResolver = nullptr;
    }
  }

  NS_IMETHODIMP
  Run()
  {
    AssertIsOnMainThread();
    // AddFeature() call failed, don't bother running.
    if (!mResolver) {
      return NS_OK;
    }

    nsCOMPtr<nsIPrincipal> principal = mResolver->GetWorkerPrivate()->GetPrincipal();
    nsRefPtr<FetchDriver> fetch = new FetchDriver(mRequest, principal);
    nsresult rv = fetch->Fetch(mResolver);
    // Right now we only support async fetch, which should never directly fail.
    MOZ_ASSERT(NS_SUCCEEDED(rv));
    if (NS_WARN_IF(NS_FAILED(rv))) {
      return rv;
    }
    return NS_OK;
  }
};

already_AddRefed<Promise>
FetchRequest(nsIGlobalObject* aGlobal, const RequestOrScalarValueString& aInput,
             const RequestInit& aInit, ErrorResult& aRv)
{
  nsRefPtr<Promise> p = Promise::Create(aGlobal, aRv);
  if (NS_WARN_IF(aRv.Failed())) {
    return nullptr;
  }

  AutoJSAPI jsapi;
  jsapi.Init(aGlobal);
  JSContext* cx = jsapi.cx();

  JS::Rooted<JSObject*> jsGlobal(cx, aGlobal->GetGlobalJSObject());
  GlobalObject global(cx, jsGlobal);

  nsRefPtr<Request> request = Request::Constructor(global, aInput, aInit, aRv);
  if (NS_WARN_IF(aRv.Failed())) {
    return nullptr;
  }

  nsRefPtr<InternalRequest> r = request->GetInternalRequest();
  if (!r->ReferrerIsNone()) {
    nsAutoCString ref;
    aRv = GetRequestReferrer(aGlobal, r, ref);
    if (NS_WARN_IF(aRv.Failed())) {
      return nullptr;
    }
    r->SetReferrer(ref);
  }

  if (NS_IsMainThread()) {
    nsCOMPtr<nsPIDOMWindow> window = do_QueryInterface(aGlobal);
    if (!window) {
      NS_WARNING("DOMFetch() aGlobal on main thread should be a window");
      aRv.Throw(NS_ERROR_FAILURE);
      return nullptr;
    }

    nsCOMPtr<nsIDocument> doc = window->GetExtantDoc();
    if (!doc) {
      NS_WARNING("DOMFetch() window does not have valid document");
      aRv.Throw(NS_ERROR_FAILURE);
      return nullptr;
    }

    nsRefPtr<MainThreadFetchResolver> resolver = new MainThreadFetchResolver(p);
    nsRefPtr<FetchDriver> fetch = new FetchDriver(r, doc->NodePrincipal());
    aRv = fetch->Fetch(resolver);
    if (NS_WARN_IF(aRv.Failed())) {
      return nullptr;
    }
  } else {
    WorkerPrivate* worker = GetCurrentThreadWorkerPrivate();
    MOZ_ASSERT(worker);
    nsRefPtr<MainThreadFetchRunnable> run = new MainThreadFetchRunnable(worker, p, r);
    if (NS_FAILED(NS_DispatchToMainThread(run))) {
      NS_WARNING("MainThreadFetchRunnable dispatch failed!");
    }
  }

  return p.forget();
}

MainThreadFetchResolver::MainThreadFetchResolver(Promise* aPromise)
  : mPromise(aPromise)
{
}

void
MainThreadFetchResolver::OnResponseAvailable(InternalResponse* aResponse)
{
  NS_ASSERT_OWNINGTHREAD(MainThreadFetchResolver);
  AssertIsOnMainThread();

  nsCOMPtr<nsIGlobalObject> go = mPromise->GetParentObject();
  mResponse = new Response(go, aResponse);
  mPromise->MaybeResolve(mResponse);
}

void
MainThreadFetchResolver::OnResponseEnd()
{
  NS_ASSERT_OWNINGTHREAD(MainThreadFetchResolver);
  AssertIsOnMainThread();
  MOZ_ASSERT(mResponse);
  mResponse->TryFinishConsumeBody();
}

MainThreadFetchResolver::~MainThreadFetchResolver()
{
  NS_ASSERT_OWNINGTHREAD(MainThreadFetchResolver);
}

class WorkerFetchResponseRunnable : public WorkerRunnable
{
  nsRefPtr<WorkerFetchResolver> mResolver;
  // Passed from main thread to worker thread after being initialized.
  // The InternalResponse's body is set at some later time on the main thread.
  // It is safe to not synchronize this because the content exposed Response
  // object is only told to TryFinishConsumeBody() on the worker thread after
  // a ResponseEnd() has been fired by the FetchDriver.
  nsRefPtr<InternalResponse> mInternalResponse;
public:
  WorkerFetchResponseRunnable(WorkerFetchResolver* aResolver, InternalResponse* aResponse)
    : WorkerRunnable(aResolver->GetWorkerPrivate(), WorkerThreadModifyBusyCount)
    , mResolver(aResolver)
    , mInternalResponse(aResponse)
  {
  }

  bool
  WorkerRun(JSContext* aCx, WorkerPrivate* aWorkerPrivate) MOZ_OVERRIDE
  {
    MOZ_ASSERT(aWorkerPrivate);
    aWorkerPrivate->AssertIsOnWorkerThread();
    MOZ_ASSERT(aWorkerPrivate == mResolver->GetWorkerPrivate());

    nsRefPtr<nsIGlobalObject> global = aWorkerPrivate->GlobalScope();
    mResolver->mResponse = new Response(global, mInternalResponse);

    nsRefPtr<Promise> promise = mResolver->mFetchPromise.forget();
    promise->MaybeResolve(mResolver->mResponse);

    return true;
  }
};

class WorkerFetchResponseEndRunnable : public WorkerRunnable
{
  nsRefPtr<WorkerFetchResolver> mResolver;
public:
  WorkerFetchResponseEndRunnable(WorkerFetchResolver* aResolver)
    : WorkerRunnable(aResolver->GetWorkerPrivate(), WorkerThreadModifyBusyCount)
    , mResolver(aResolver)
  {
  }

  bool
  WorkerRun(JSContext* aCx, WorkerPrivate* aWorkerPrivate) MOZ_OVERRIDE
  {
    MOZ_ASSERT(aWorkerPrivate);
    aWorkerPrivate->AssertIsOnWorkerThread();
    MOZ_ASSERT(aWorkerPrivate == mResolver->GetWorkerPrivate());
    MOZ_ASSERT(mResolver->mResponse);

    mResolver->mResponse->TryFinishConsumeBody();
    mResolver->CleanUp(aCx);
    return true;
  }
};

void
WorkerFetchResolver::OnResponseAvailable(InternalResponse* aResponse)
{
  AssertIsOnMainThread();

  MutexAutoLock lock(mCleanUpLock);
  if (mCleanedUp) {
    NS_WARNING("Worker already stopped running");
    return;
  }

  nsRefPtr<WorkerFetchResponseRunnable> r =
    new WorkerFetchResponseRunnable(this, aResponse);

  AutoSafeJSContext cx;
  r->Dispatch(cx);
}

void
WorkerFetchResolver::OnResponseEnd()
{
  AssertIsOnMainThread();
  MutexAutoLock lock(mCleanUpLock);
  if (mCleanedUp) {
    NS_WARNING("Worker already stopped running");
    return;
  }

  nsRefPtr<WorkerFetchResponseEndRunnable> r =
    new WorkerFetchResponseEndRunnable(this);

  AutoSafeJSContext cx;
  if (!r->Dispatch(cx)) {
    NS_WARNING("Could not dispatch fetch resolve");
  }
}

// Empty string for no-referrer. FIXME(nsm): Does returning empty string
// actually lead to no-referrer in the base channel?
// The actual referrer policy and stripping is dealt with by HttpBaseChannel,
// this always returns the full API referrer URL of the relevant global.
nsresult
GetRequestReferrer(nsIGlobalObject* aGlobal, const InternalRequest* aRequest, nsACString& aReferrer)
{
  if (aRequest->ReferrerIsURL()) {
    aReferrer = aRequest->ReferrerAsURL();
    return NS_OK;
  }
  nsCOMPtr<nsPIDOMWindow> window = do_QueryInterface(aGlobal);
  if (window) {
    nsCOMPtr<nsIDocument> doc = window->GetExtantDoc();
    if (doc) {
      nsCOMPtr<nsIURI> docURI = doc->GetDocumentURI();
      nsAutoCString origin;
      nsresult rv = nsContentUtils::GetASCIIOrigin(docURI, origin);
      if (NS_WARN_IF(NS_FAILED(rv))) {
        return rv;
      }

      nsAutoString referrer;
      doc->GetReferrer(referrer);
      aReferrer = NS_ConvertUTF16toUTF8(referrer);
    }
  } else {
    WorkerPrivate* worker = GetCurrentThreadWorkerPrivate();
    MOZ_ASSERT(worker);
    worker->AssertIsOnWorkerThread();
    aReferrer = worker->GetLocationInfo().mHref;
    // XXX(nsm): Algorithm says "If source is not a URL..." but when is it
    // not a URL?
  }

  return NS_OK;
}

namespace {
nsresult
ExtractFromArrayBuffer(const ArrayBuffer& aBuffer,
                       nsIInputStream** aStream)
{
  aBuffer.ComputeLengthAndData();
  //XXXnsm reinterpret_cast<> is used in DOMParser, should be ok.
  return NS_NewByteInputStream(aStream,
                               reinterpret_cast<char*>(aBuffer.Data()),
                               aBuffer.Length(), NS_ASSIGNMENT_COPY);
}

nsresult
ExtractFromArrayBufferView(const ArrayBufferView& aBuffer,
                           nsIInputStream** aStream)
{
  aBuffer.ComputeLengthAndData();
  //XXXnsm reinterpret_cast<> is used in DOMParser, should be ok.
  return NS_NewByteInputStream(aStream,
                               reinterpret_cast<char*>(aBuffer.Data()),
                               aBuffer.Length(), NS_ASSIGNMENT_COPY);
}

nsresult
ExtractFromBlob(const File& aFile, nsIInputStream** aStream,
                nsCString& aContentType)
{
  nsRefPtr<FileImpl> impl = aFile.Impl();
  nsresult rv = impl->GetInternalStream(aStream);
  if (NS_WARN_IF(NS_FAILED(rv))) {
    return rv;
  }

  nsString type;
  impl->GetType(type);
  aContentType = NS_ConvertUTF16toUTF8(type);
  return NS_OK;
}

nsresult
ExtractFromScalarValueString(const nsString& aStr,
                             nsIInputStream** aStream,
                             nsCString& aContentType)
{
  nsCOMPtr<nsIUnicodeEncoder> encoder = EncodingUtils::EncoderForEncoding("UTF-8");
  if (!encoder) {
    return NS_ERROR_OUT_OF_MEMORY;
  }

  int32_t destBufferLen;
  nsresult rv = encoder->GetMaxLength(aStr.get(), aStr.Length(), &destBufferLen);
  if (NS_WARN_IF(NS_FAILED(rv))) {
    return rv;
  }

  nsCString encoded;
  if (!encoded.SetCapacity(destBufferLen, fallible_t())) {
    return NS_ERROR_OUT_OF_MEMORY;
  }

  char* destBuffer = encoded.BeginWriting();
  int32_t srcLen = (int32_t) aStr.Length();
  int32_t outLen = destBufferLen;
  rv = encoder->Convert(aStr.get(), &srcLen, destBuffer, &outLen);
  if (NS_WARN_IF(NS_FAILED(rv))) {
    return rv;
  }

  MOZ_ASSERT(outLen <= destBufferLen);
  encoded.SetLength(outLen);

  aContentType = NS_LITERAL_CSTRING("text/plain;charset=UTF-8");

  return NS_NewCStringInputStream(aStream, encoded);
}

nsresult
ExtractFromURLSearchParams(const URLSearchParams& aParams,
                           nsIInputStream** aStream,
                           nsCString& aContentType)
{
  nsAutoString serialized;
  aParams.Stringify(serialized);
  aContentType = NS_LITERAL_CSTRING("application/x-www-form-urlencoded;charset=UTF-8");
  return NS_NewStringInputStream(aStream, serialized);
}
}

nsresult
ExtractByteStreamFromBody(const OwningArrayBufferOrArrayBufferViewOrBlobOrScalarValueStringOrURLSearchParams& aBodyInit,
                          nsIInputStream** aStream,
                          nsCString& aContentType)
{
  MOZ_ASSERT(aStream);

  if (aBodyInit.IsArrayBuffer()) {
    const ArrayBuffer& buf = aBodyInit.GetAsArrayBuffer();
    return ExtractFromArrayBuffer(buf, aStream);
  } else if (aBodyInit.IsArrayBufferView()) {
    const ArrayBufferView& buf = aBodyInit.GetAsArrayBufferView();
    return ExtractFromArrayBufferView(buf, aStream);
  } else if (aBodyInit.IsBlob()) {
    const File& blob = aBodyInit.GetAsBlob();
    return ExtractFromBlob(blob, aStream, aContentType);
  } else if (aBodyInit.IsScalarValueString()) {
    nsAutoString str;
    str.Assign(aBodyInit.GetAsScalarValueString());
    return ExtractFromScalarValueString(str, aStream, aContentType);
  } else if (aBodyInit.IsURLSearchParams()) {
    URLSearchParams& params = aBodyInit.GetAsURLSearchParams();
    return ExtractFromURLSearchParams(params, aStream, aContentType);
  }

  NS_NOTREACHED("Should never reach here");
  return NS_ERROR_FAILURE;
}

nsresult
ExtractByteStreamFromBody(const ArrayBufferOrArrayBufferViewOrBlobOrScalarValueStringOrURLSearchParams& aBodyInit,
                          nsIInputStream** aStream,
                          nsCString& aContentType)
{
  MOZ_ASSERT(aStream);

  if (aBodyInit.IsArrayBuffer()) {
    const ArrayBuffer& buf = aBodyInit.GetAsArrayBuffer();
    return ExtractFromArrayBuffer(buf, aStream);
  } else if (aBodyInit.IsArrayBufferView()) {
    const ArrayBufferView& buf = aBodyInit.GetAsArrayBufferView();
    return ExtractFromArrayBufferView(buf, aStream);
  } else if (aBodyInit.IsBlob()) {
    const File& blob = aBodyInit.GetAsBlob();
    return ExtractFromBlob(blob, aStream, aContentType);
  } else if (aBodyInit.IsScalarValueString()) {
    nsAutoString str;
    str.Assign(aBodyInit.GetAsScalarValueString());
    return ExtractFromScalarValueString(str, aStream, aContentType);
  } else if (aBodyInit.IsURLSearchParams()) {
    URLSearchParams& params = aBodyInit.GetAsURLSearchParams();
    return ExtractFromURLSearchParams(params, aStream, aContentType);
  }

  NS_NOTREACHED("Should never reach here");
  return NS_ERROR_FAILURE;
}

namespace {
class StreamDecoder MOZ_FINAL
{
  nsCOMPtr<nsIUnicodeDecoder> mDecoder;
  nsString mDecoded;

public:
  StreamDecoder()
    : mDecoder(EncodingUtils::DecoderForEncoding("UTF-8"))
  {
    MOZ_ASSERT(mDecoder);
  }

  nsresult
  AppendText(const char* aSrcBuffer, uint32_t aSrcBufferLen)
  {
    int32_t destBufferLen;
    nsresult rv =
      mDecoder->GetMaxLength(aSrcBuffer, aSrcBufferLen, &destBufferLen);
    if (NS_WARN_IF(NS_FAILED(rv))) {
      return rv;
    }

    if (!mDecoded.SetCapacity(mDecoded.Length() + destBufferLen, fallible_t())) {
      return NS_ERROR_OUT_OF_MEMORY;
    }

    char16_t* destBuffer = mDecoded.BeginWriting() + mDecoded.Length();
    int32_t totalChars = mDecoded.Length();

    int32_t srcLen = (int32_t) aSrcBufferLen;
    int32_t outLen = destBufferLen;
    rv = mDecoder->Convert(aSrcBuffer, &srcLen, destBuffer, &outLen);
    MOZ_ASSERT(NS_SUCCEEDED(rv));

    totalChars += outLen;
    mDecoded.SetLength(totalChars);

    return NS_OK;
  }

  nsString&
  GetText()
  {
    return mDecoded;
  }
};

// Copied from nsXMLHttpRequest.cpp.
// Maximum size that we'll grow an ArrayBuffer instead of doubling,
// once doubling reaches this threshold
#define FETCH_ARRAYBUFFER_MAX_GROWTH (32*1024*1024)
// start at 32k to avoid lots of doubling right at the start
#define FETCH_ARRAYBUFFER_MIN_SIZE (32*1024)

NS_METHOD
FillArrayBuffer(nsIInputStream* in,
                void* closure,
                const char* fromRawSegment,
                uint32_t toOffset,
                uint32_t count,
                uint32_t *writeCount)
{
  ArrayBufferBuilder* builder = static_cast<ArrayBufferBuilder*>(closure);
  MOZ_ASSERT(builder);
  if (builder->capacity() == 0) {
    builder->setCapacity(PR_MAX(count, FETCH_ARRAYBUFFER_MIN_SIZE));
  }

  if (!builder->append(reinterpret_cast<const uint8_t*>(fromRawSegment), count, FETCH_ARRAYBUFFER_MAX_GROWTH)) {
    *writeCount = 0;
    return NS_ERROR_OUT_OF_MEMORY;
  }

  *writeCount = count;
  return NS_OK;
}

NS_METHOD
FillBlobSet(nsIInputStream* in,
            void* closure,
            const char* fromRawSegment,
            uint32_t toOffset,
            uint32_t count,
            uint32_t *writeCount)
{
  BlobSet* blobSet = static_cast<BlobSet*>(closure);
  MOZ_ASSERT(blobSet);
  nsresult rv = blobSet->AppendVoidPtr(fromRawSegment, count);
  if (NS_WARN_IF(NS_FAILED(rv))) {
    *writeCount = 0;
  } else {
    *writeCount = count;
  }
  return rv;
}

NS_METHOD
FillUTF8Decoded(nsIInputStream* in,
                void* closure,
                const char* fromRawSegment,
                uint32_t toOffset,
                uint32_t count,
                uint32_t *writeCount)
{
  StreamDecoder* decoder = static_cast<StreamDecoder*>(closure);
  MOZ_ASSERT(decoder);

  nsresult rv = decoder->AppendText(fromRawSegment, count);
  if (NS_WARN_IF(NS_FAILED(rv))) {
    *writeCount = 0;
  } else {
    *writeCount = count;
  }
  return rv;
}
}

template <class Derived>
void
FetchBody<Derived>::FinishConsumeBody()
{
  nsCOMPtr<nsIInputStream> stream;
  DerivedClass()->GetBody(getter_AddRefs(stream));
  MOZ_ASSERT(stream);

  if (!NS_InputStreamIsBuffered(stream)) {
    nsCOMPtr<nsIInputStream> buffered;
    nsresult rv = NS_NewBufferedInputStream(getter_AddRefs(buffered),
                                            stream, 4096);
    if (NS_WARN_IF(NS_FAILED(rv))) {
      return mDelayedConsumePromise->MaybeReject(rv);
    }

    stream = buffered.forget();
  }

  uint64_t len;
  nsresult rv = stream->Available(&len);
  if (NS_WARN_IF(NS_FAILED(rv))) {
    return mDelayedConsumePromise->MaybeReject(rv);
  }

  AutoJSAPI api;
  api.Init(DerivedClass()->GetParentObject());
  JSContext* cx = api.cx();

  uint32_t totalRead;
  switch (mDelayedConsumeType) {
    case CONSUME_ARRAYBUFFER: {
      mozilla::ArrayBufferBuilder builder;
      rv = stream->ReadSegments(FillArrayBuffer, &builder, len, &totalRead);
      if (NS_WARN_IF(NS_FAILED(rv))) {
        return mDelayedConsumePromise->MaybeReject(rv);
      }
      JS::Rooted<JSObject*> arrayBuffer(cx, builder.getArrayBuffer(cx));
      JS::Rooted<JS::Value> val(cx);
      val.setObjectOrNull(arrayBuffer);
      return mDelayedConsumePromise->MaybeResolve(cx, val);
    }
    case CONSUME_BLOB: {
      // The input stream is backed by various blobs when obtained from HTTP.
      // It would be nice if we could create a Blob that just adopted/shared
      // that data instead of copying it.
      nsAutoPtr<BlobSet> blobSet(new BlobSet());
      rv = stream->ReadSegments(FillBlobSet, blobSet, len, &totalRead);
      if (NS_WARN_IF(NS_FAILED(rv))) {
        return mDelayedConsumePromise->MaybeReject(rv);
      }

      nsRefPtr<File> blob =
        blobSet->GetBlobInternal(DerivedClass()->GetParentObject(), mMimeType);
      return mDelayedConsumePromise->MaybeResolve(blob);
    }
    case CONSUME_TEXT:
      // fall through handles early exit.
    case CONSUME_JSON: {
      StreamDecoder decoder;
      rv = stream->ReadSegments(FillUTF8Decoded, &decoder, len, &totalRead);
      if (NS_WARN_IF(NS_FAILED(rv))) {
        return mDelayedConsumePromise->MaybeReject(rv);
      }

      if (mDelayedConsumeType == CONSUME_TEXT) {
        return mDelayedConsumePromise->MaybeResolve(decoder.GetText());
      }

      nsString decoded = decoder.GetText();
      JS::Rooted<JS::Value> json(cx);
      if (!JS_ParseJSON(cx, decoded.get(), decoded.Length(), &json)) {
        JS::Rooted<JS::Value> exn(cx);
        if (JS_GetPendingException(cx, &exn)) {
          JS_ClearPendingException(cx);
          mDelayedConsumePromise->MaybeReject(cx, exn);
        }
      }

      return mDelayedConsumePromise->MaybeResolve(cx, json);
    }
  }

  NS_NOTREACHED("Unexpected consume body type");
}

template <class Derived>
void
FetchBody<Derived>::TryFinishConsumeBody()
{
  if (mDelayedConsumePromise) {
    FinishConsumeBody();
    mDelayedConsumePromise = nullptr;
  }
}

template <class Derived>
already_AddRefed<Promise>
FetchBody<Derived>::ConsumeBody(ConsumeType aType, ErrorResult& aRv)
{
  nsRefPtr<Promise> promise = Promise::Create(DerivedClass()->GetParentObject(), aRv);
  if (aRv.Failed()) {
    return nullptr;
  }

  if (BodyUsed()) {
    aRv.ThrowTypeError(MSG_REQUEST_BODY_CONSUMED_ERROR);
    return nullptr;
  }

  SetBodyUsed();

  mDelayedConsumePromise = promise;
  mDelayedConsumeType = aType;

  nsCOMPtr<nsIInputStream> stream;
  DerivedClass()->GetBody(getter_AddRefs(stream));
  // If the stream is available, we immediately read data from it (at this
  // point it is memory backed and so we can just read it on the main/worker
  // thread.)
  if (stream) {
    FinishConsumeBody();
  }

  return promise.forget();
}

template
already_AddRefed<Promise>
FetchBody<Request>::ConsumeBody(ConsumeType aType, ErrorResult& aRv);

template
already_AddRefed<Promise>
FetchBody<Response>::ConsumeBody(ConsumeType aType, ErrorResult& aRv);

template <class Derived>
void
FetchBody<Derived>::SetMimeType(ErrorResult& aRv)
{
  // Extract mime type.
  nsTArray<nsCString> contentTypeValues;
  MOZ_ASSERT(DerivedClass()->GetInternalHeaders());
  DerivedClass()->GetInternalHeaders()->GetAll(NS_LITERAL_CSTRING("Content-Type"), contentTypeValues, aRv);
  if (NS_WARN_IF(aRv.Failed())) {
    return;
  }

  // HTTP ABNF states Content-Type may have only one value.
  // This is from the "parse a header value" of the fetch spec.
  if (contentTypeValues.Length() == 1) {
    mMimeType = contentTypeValues[0];
    ToLowerCase(mMimeType);
  }
}

template
void
FetchBody<Request>::SetMimeType(ErrorResult& aRv);

template
void
FetchBody<Response>::SetMimeType(ErrorResult& aRv);
} // namespace dom
} // namespace mozilla
