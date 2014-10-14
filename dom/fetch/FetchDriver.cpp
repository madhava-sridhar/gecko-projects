/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "mozilla/dom/FetchDriver.h"

#include "nsIDocument.h"
#include "nsIAsyncInputStream.h"
#include "nsIAsyncOutputStream.h"
#include "nsIHttpChannel.h"
#include "nsIHttpHeaderVisitor.h"
#include "nsIScriptSecurityManager.h"
#include "nsIThreadRetargetableRequest.h"
#include "nsIUploadChannel2.h"

#include "nsContentPolicyUtils.h"
#include "nsDataHandler.h"
#include "nsHostObjectProtocolHandler.h"
#include "nsNetUtil.h"
#include "nsStringStream.h"

#include "mozilla/dom/File.h"
#include "mozilla/dom/workers/Workers.h"

#include "Fetch.h"
#include "InternalRequest.h"
#include "InternalResponse.h"

namespace mozilla {
namespace dom {

NS_IMPL_ISUPPORTS(FetchDriver, nsIStreamListener)

FetchDriver::FetchDriver(InternalRequest* aRequest, nsIPrincipal* aPrincipal, nsIDocument* aDoc)
  : mPrincipal(aPrincipal)
  , mDocument(aDoc)
  , mRequest(aRequest)
  , mFetchRecursionCount(0)
{
}

FetchDriver::~FetchDriver()
{
}

nsresult
FetchDriver::Fetch(FetchDriverObserver* aObserver)
{
  workers::AssertIsOnMainThread();
  mObserver = aObserver;

  return Fetch(false /* CORS flag */);
}

nsresult
FetchDriver::Fetch(bool aCORSFlag)
{
  // We do not currently implement parts of the spec that lead to recursion.
  MOZ_ASSERT(mFetchRecursionCount == 0);
  mFetchRecursionCount++;

  // FIXME(nsm): Deal with HSTS.

  if (!mRequest->IsSynchronous() && mFetchRecursionCount <= 1) {
    nsCOMPtr<nsIRunnable> r =
      NS_NewRunnableMethodWithArg<bool>(this, &FetchDriver::ContinueFetch, aCORSFlag);
    return NS_DispatchToCurrentThread(r);
  }

  MOZ_CRASH("Synchronous fetch not supported");
}

nsresult
FetchDriver::ContinueFetch(bool aCORSFlag)
{
  workers::AssertIsOnMainThread();

  nsAutoCString url;
  mRequest->GetURL(url);
  nsCOMPtr<nsIURI> requestURI;
  // FIXME(nsm): Deal with relative URLs.
  nsresult rv = NS_NewURI(getter_AddRefs(requestURI), url,
                          nullptr, nullptr);
  if (NS_WARN_IF(NS_FAILED(rv))) {
    return FailWithNetworkError();
  }

  // CSP/mixed content checks.
  int16_t shouldLoad;
  rv = NS_CheckContentLoadPolicy(mRequest->GetContext(),
                                 requestURI,
                                 mPrincipal,
                                 mDocument,
                                 // FIXME(nsm): Should MIME be extracted from
                                 // Content-Type header?
                                 EmptyCString(), /* mime guess */
                                 nullptr, /* extra */
                                 &shouldLoad,
                                 nsContentUtils::GetContentPolicy(),
                                 nsContentUtils::GetSecurityManager());
  if (NS_WARN_IF(NS_FAILED(rv)) || NS_CP_REJECTED(shouldLoad)) {
    // Disallowed by content policy.
    return FailWithNetworkError();
  }

  nsAutoCString scheme;
  rv = requestURI->GetScheme(scheme);
  if (NS_WARN_IF(NS_FAILED(rv))) {
    return FailWithNetworkError();
  }

  nsAutoCString originURL;
  mRequest->GetOrigin(originURL);
  nsCOMPtr<nsIURI> originURI;
  rv = NS_NewURI(getter_AddRefs(originURI), originURL, nullptr, nullptr);
  if (NS_WARN_IF(NS_FAILED(rv))) {
    return FailWithNetworkError();
  }

  nsIScriptSecurityManager* ssm = nsContentUtils::GetSecurityManager();
  rv = ssm->CheckSameOriginURI(requestURI, originURI, false);
  if ((!aCORSFlag && NS_SUCCEEDED(rv)) ||
      (scheme.EqualsLiteral("data") && mRequest->SameOriginDataURL()) ||
      scheme.EqualsLiteral("about")) {
    return BasicFetch();
  }

  if (mRequest->Mode() == RequestMode::Same_origin) {
    return FailWithNetworkError();
  }

  if (mRequest->Mode() == RequestMode::No_cors) {
    mRequest->SetResponseTainting(InternalRequest::RESPONSETAINT_OPAQUE);
    return BasicFetch();
  }

  if (!scheme.EqualsLiteral("http") && !scheme.EqualsLiteral("https")) {
    return FailWithNetworkError();
  }

  bool corsPreflight = false;
  if (mRequest->Mode() == RequestMode::Cors_with_forced_preflight ||
      (mRequest->UnsafeRequest() && (mRequest->HasSimpleMethod() || !mRequest->Headers()->HasOnlySimpleHeaders()))) {
    corsPreflight = true;
  }

  mRequest->SetResponseTainting(InternalRequest::RESPONSETAINT_CORS);
  return HttpFetch(true /* aCORSFlag */, corsPreflight);
}

nsresult
FetchDriver::BasicFetch()
{
  nsAutoCString url;
  mRequest->GetURL(url);
  nsCOMPtr<nsIURI> uri;
  nsresult rv = NS_NewURI(getter_AddRefs(uri),
                 url,
                 nullptr,
                 nullptr);
  NS_ENSURE_SUCCESS(rv, rv);

  nsCString scheme;
  rv = uri->GetScheme(scheme);
  NS_ENSURE_SUCCESS(rv, rv);

  if (scheme.LowerCaseEqualsLiteral("about")) {
    if (url.EqualsLiteral("about:blank")) {
      nsRefPtr<InternalResponse> response =
        new InternalResponse(200, NS_LITERAL_CSTRING("OK"));
      ErrorResult result;
      response->Headers()->Append(NS_LITERAL_CSTRING("content-type"),
                                  NS_LITERAL_CSTRING("text/html;charset=utf-8"),
                                  result);
      MOZ_ASSERT(!result.Failed());
      nsCOMPtr<nsIInputStream> body;
      rv = NS_NewCStringInputStream(getter_AddRefs(body), EmptyCString());
      if (NS_WARN_IF(NS_FAILED(rv))) {
        return FailWithNetworkError();
      }

      response->SetBody(body);
      BeginResponse(response);
      return SucceedWithResponse();
    }
    return FailWithNetworkError();
  }

  if (scheme.LowerCaseEqualsLiteral("blob")) {
    nsRefPtr<FileImpl> blobImpl;
    rv = NS_GetBlobForBlobURI(uri, getter_AddRefs(blobImpl));
    FileImpl* blob = static_cast<FileImpl*>(blobImpl.get());
    if (NS_WARN_IF(NS_FAILED(rv))) {
      return FailWithNetworkError();
    }

    nsRefPtr<InternalResponse> response = new InternalResponse(200, NS_LITERAL_CSTRING("OK"));
    {
      ErrorResult result;
      uint64_t size = blob->GetSize(result);
      if (NS_WARN_IF(result.Failed())) {
        return FailWithNetworkError();
      }

      nsAutoString sizeStr;
      sizeStr.AppendInt(size);
      response->Headers()->Append(NS_LITERAL_CSTRING("Content-Length"), NS_ConvertUTF16toUTF8(sizeStr), result);
      if (NS_WARN_IF(result.Failed())) {
        return FailWithNetworkError();
      }

      nsAutoString type;
      blob->GetType(type);
      response->Headers()->Append(NS_LITERAL_CSTRING("Content-Type"), NS_ConvertUTF16toUTF8(type), result);
      if (NS_WARN_IF(result.Failed())) {
        return FailWithNetworkError();
      }
    }

    nsCOMPtr<nsIInputStream> stream;
    rv = blob->GetInternalStream(getter_AddRefs(stream));
    if (NS_WARN_IF(NS_FAILED(rv))) {
      return FailWithNetworkError();
    }

    response->SetBody(stream);
    BeginResponse(response);
    return SucceedWithResponse();
  }

  if (scheme.LowerCaseEqualsLiteral("data")) {
    nsAutoCString method;
    mRequest->GetMethod(method);
    if (method.LowerCaseEqualsASCII("get")) {
      // Use nsDataHandler directly so that we can extract the content type.
      // XXX(nsm): Is there a way to acquire the charset without such tight
      // coupling with the DataHandler? nsIProtocolHandler does not provide
      // anything similar.
      nsAutoCString contentType, contentCharset, dataBuffer, hashRef;
      bool isBase64;
      rv = nsDataHandler::ParseURI(url,
                                   contentType,
                                   contentCharset,
                                   isBase64,
                                   dataBuffer,
                                   hashRef);
      if (NS_SUCCEEDED(rv)) {
        ErrorResult result;
        nsRefPtr<InternalResponse> response = new InternalResponse(200, NS_LITERAL_CSTRING("OK"));
        if (!contentCharset.IsEmpty()) {
          contentType.Append(";charset=");
          contentType.Append(contentCharset);
        }

        response->Headers()->Append(NS_LITERAL_CSTRING("Content-Type"), contentType, result);
        if (!result.Failed()) {
          nsCOMPtr<nsIInputStream> stream;
          rv = NS_NewCStringInputStream(getter_AddRefs(stream), dataBuffer);
          if (NS_SUCCEEDED(rv)) {
            response->SetBody(stream);
            BeginResponse(response);
            return SucceedWithResponse();
          }
        }
      }
    }

    return FailWithNetworkError();
  }

  if (scheme.LowerCaseEqualsLiteral("file")) {
  } else if (scheme.LowerCaseEqualsLiteral("http") ||
             scheme.LowerCaseEqualsLiteral("https")) {
    return HttpFetch();
  }

  return FailWithNetworkError();
}

nsresult
FetchDriver::HttpFetch(bool aCORSFlag, bool aCORSPreflightFlag, bool aAuthenticationFlag)
{
  mResponse = nullptr;

  // FIXME(nsm): See if ServiceWorker can handle it.
  return ContinueHttpFetchAfterServiceWorker();
}

nsresult
FetchDriver::ContinueHttpFetchAfterServiceWorker()
{
  if (!mResponse) {
    // FIXME(nsm): I believe the method cache match stuff should be handled
    // by our CORS handling code.
    if (aCORSPreflightFlag &&
        (!mRequest->HasSimpleMethod() || mRequest->Mode() == RequestMode::Cors_with_forced_preflight) &&
        !mRequest->Headers()->HasOnlySimpleHeaders()) {
      nsresult rv = BeginCORSPreflight();
      if (NS_WARN_IF(NS_FAILED(rv))) {
        return FailWithNetworkError();
      }
      return NS_OK;
    }
    // FIXME(nsm): Set skip SW flag.
    // FIXME(nsm): Deal with CORS flags cases which will also call
    // ContinueHttpFetchAfterCORSPreflight().
    return ContinueHttpFetchAfterCORSPreflight();
  }

  // Otherwise ServiceWorker replied with a response.
  return ContinueHttpFetchAfterNetworkFetch();
}

nsresult
FetchDriver::ContinueHttpFetchAfterCORSPreflight()
{
  // mResponse is currently the CORS response.
  // We may have to pass it via argument.
  if (mResponse && mResponse->IsError()) {
    return FailWithNetworkError();
  }

  return HttpNetworkFetch();
}

nsresult
FetchDriver::HttpNetworkFetch()
{
  // We don't create a HTTPRequest copy since Necko sets the information on the
  // nsIHttpChannel instead.
  // FIXME(nsm): Figure out how to tee request's body.

  // FIXME(nsm): Http network fetch steps 2-7.
  nsresult rv;

  nsCOMPtr<nsIIOService> ios = do_GetIOService(&rv);
  NS_ENSURE_SUCCESS(rv, rv);

  nsCString url;
  mRequest->GetURL(url);
  nsCOMPtr<nsIURI> uri;
  rv = NS_NewURI(getter_AddRefs(uri),
                          url,
                          nullptr,
                          nullptr,
                          ios);
  NS_ENSURE_SUCCESS(rv, rv);

  nsCOMPtr<nsIChannel> chan;
  nsCOMPtr<nsIChannelPolicy> channelPolicy;
  nsCOMPtr<nsIContentSecurityPolicy> csp;
  rv = mPrincipal->GetCsp(getter_AddRefs(csp));
  if (NS_WARN_IF(NS_FAILED(rv))) {
    return FailWithNetworkError();
  }

  if (csp) {
    channelPolicy = do_CreateInstance("@mozilla.org/nschannelpolicy;1");
    channelPolicy->SetContentSecurityPolicy(csp);
    channelPolicy->SetLoadType(mRequest->GetContext());
  }

  rv = NS_NewChannel(getter_AddRefs(chan),
                     uri,
                     mPrincipal,
                     nsILoadInfo::SEC_NORMAL,
                     mRequest->GetContext(),
                     nullptr, /* FIXME(nsm): loadgroup */
                     nullptr, /* aCallbacks */
                     nsIRequest::LOAD_BACKGROUND,
                     ios);
  NS_ENSURE_SUCCESS(rv, rv);

  nsCOMPtr<nsIHttpChannel> httpChan = do_QueryInterface(chan);
  if (httpChan) {
    nsCString method;
    mRequest->GetMethod(method);
    rv = httpChan->SetRequestMethod(method);
    if (NS_WARN_IF(NS_FAILED(rv))) {
      return FailWithNetworkError();
    }

    nsTArray<InternalHeaders::Entry> headers;
    mRequest->Headers()->GetEntries(headers);
    for (size_t i = 0; i < headers.Length(); ++i) {
      httpChan->SetRequestHeader(headers[i].mName, headers[i].mValue, false /* merge */);
    }

    MOZ_ASSERT(mRequest->ReferrerIsURL());
    nsCString referrer = mRequest->ReferrerAsURL();
    if (!referrer.IsEmpty()) {
      nsCOMPtr<nsIURI> uri;
      rv = NS_NewURI(getter_AddRefs(uri), referrer, nullptr, nullptr, ios);
      if (NS_WARN_IF(NS_FAILED(rv))) {
        return rv;
      }
      rv = httpChan->SetReferrer(uri);
      if (NS_WARN_IF(NS_FAILED(rv))) {
        return rv;
      }
    }

    if (mRequest->ForceOriginHeader()) {
      nsCString origin;
      mRequest->GetOrigin(origin);
      httpChan->SetRequestHeader(NS_LITERAL_CSTRING("origin"),
                                 origin,
                                 false /* merge */);
    }

    // FIXME(nsm): Step 4 credentials.
    // FIXME(nsm): Step 5 proxy auth entry.
    // FIXME(nsm): Step 6. I don't think we have to do this here. Necko should
    // handle it.
  }

  nsCOMPtr<nsIUploadChannel2> uploadChan = do_QueryInterface(chan);
  if (uploadChan) {
    nsCString contentType;
    ErrorResult result;
    mRequest->Headers()->Get(NS_LITERAL_CSTRING("content-type"), contentType, result);
    if (result.Failed()) {
      return result.ErrorCode();
    }

    nsCOMPtr<nsIInputStream> bodyStream;
    mRequest->GetBody(getter_AddRefs(bodyStream));
    if (bodyStream) {
      nsCString method;
      mRequest->GetMethod(method);
      rv = uploadChan->ExplicitSetUploadStream(bodyStream, contentType, -1, method, false /* aStreamHasHeaders */);
      if (NS_WARN_IF(NS_FAILED(rv))) {
        return rv;
      }
    }
  }
  return chan->AsyncOpen(this, nullptr);
}

nsresult
FetchDriver::ContinueHttpFetchAfterNetworkFetch()
{
  workers::AssertIsOnMainThread();
  MOZ_ASSERT(mResponse);
  MOZ_ASSERT(!mResponse->IsError());

  /*switch (mResponse->GetStatus()) {
    default:
  }*/

  return SucceedWithResponse();
}

already_AddRefed<InternalResponse>
FetchDriver::BeginAndGetFilteredResponse(InternalResponse* aResponse)
{
  MOZ_ASSERT(aResponse);
  nsAutoCString reqURL;
  mRequest->GetURL(reqURL);
  aResponse->SetUrl(reqURL);

  // FIXME(nsm): Handle mixed content check, step 7 of fetch.

  nsRefPtr<InternalResponse> filteredResponse;
  switch (mRequest->GetResponseTainting()) {
    case InternalRequest::RESPONSETAINT_BASIC:
      filteredResponse = InternalResponse::BasicResponse(aResponse);
      break;
    case InternalRequest::RESPONSETAINT_CORS:
      filteredResponse = InternalResponse::CORSResponse(aResponse);
      break;
    case InternalRequest::RESPONSETAINT_OPAQUE:
      filteredResponse = InternalResponse::OpaqueResponse();
      break;
    default:
      MOZ_CRASH("Unexpected case");
  }

  MOZ_ASSERT(filteredResponse);
  mObserver->OnResponseAvailable(filteredResponse);
  return filteredResponse.forget();
}

void
FetchDriver::BeginResponse(InternalResponse* aResponse)
{
  nsRefPtr<InternalResponse> r = BeginAndGetFilteredResponse(aResponse);
  // Release the ref.
}

nsresult
FetchDriver::SucceedWithResponse()
{
  mObserver->OnResponseEnd();
  return NS_OK;
}

nsresult
FetchDriver::FailWithNetworkError()
{
  nsRefPtr<InternalResponse> error = InternalResponse::NetworkError();
  mObserver->OnResponseAvailable(error);
  mObserver->OnResponseEnd();
  return NS_OK;
}

namespace {
class FillResponseHeaders MOZ_FINAL : public nsIHttpHeaderVisitor {
  InternalResponse* mResponse;

  ~FillResponseHeaders()
  { }
public:
  NS_DECL_ISUPPORTS

  FillResponseHeaders(InternalResponse* aResponse)
    : mResponse(aResponse)
  {
  }

  NS_IMETHOD
  VisitHeader(const nsACString & aHeader, const nsACString & aValue) MOZ_OVERRIDE
  {
    ErrorResult result;
    mResponse->Headers()->Append(aHeader, aValue, result);
    return result.ErrorCode();
  }
};

NS_IMPL_ISUPPORTS(FillResponseHeaders, nsIHttpHeaderVisitor)
} // anonymous namespace

NS_IMETHODIMP
FetchDriver::OnStartRequest(nsIRequest* aRequest,
                            nsISupports* aContext)
{
  MOZ_ASSERT(!mPipeOutputStream);
  nsresult requestStatus;
  aRequest->GetStatus(&requestStatus);
  if (NS_WARN_IF(NS_FAILED(requestStatus))) {
    return FailWithNetworkError();
  }

  nsCOMPtr<nsIHttpChannel> channel = do_QueryInterface(aRequest);
  // For now we only support HTTP.
  MOZ_ASSERT(channel);

  nsresult rv;
  aRequest->GetStatus(&rv);
  if (NS_WARN_IF(NS_FAILED(rv))) {
    nsRefPtr<InternalResponse> err = InternalResponse::NetworkError();
    mObserver->OnResponseAvailable(err);
    return rv;
  }

  uint32_t responseStatus;
  channel->GetResponseStatus(&responseStatus);

  nsCString statusText;
  channel->GetResponseStatusText(statusText);

  nsRefPtr<InternalResponse> response = new InternalResponse(responseStatus, statusText);

  nsRefPtr<FillResponseHeaders> visitor = new FillResponseHeaders(response);
  rv = channel->VisitResponseHeaders(visitor);
  if (NS_WARN_IF(NS_FAILED(rv))) {
    NS_WARNING("Failed to visit all headers.");
  }

  mResponse = BeginAndGetFilteredResponse(response);

  // We open a pipe so that we can immediately set the pipe's read end as the
  // response's body. Setting the segment size to UINT32_MAX means that the
  // pipe has infinite space, which seems odd, but this is effectively what
  // other APIs like XHR do anyway, since the entire response has to be read.
  // The difference is that in XHR it is immediately converted to text or
  // ArrayBuffer or similar, while in Fetch, we pass around streams until JS
  // requests another type.
  nsCOMPtr<nsIAsyncInputStream> pipeInputStream;
  rv = NS_NewPipe2(getter_AddRefs(pipeInputStream),
                   getter_AddRefs(mPipeOutputStream),
                   true, /* non blocking input (read) end */
                   false, /* blocking write end. Since pipe size is infinite, this won't freeze the main thread. */
                   0, /* default segment size */
                   UINT32_MAX /* infinite pipe */);
  if (NS_WARN_IF(NS_FAILED(rv))) {
    FailWithNetworkError();
    // Cancel request.
    return rv;
  }

  nsCOMPtr<nsIInputStream> cast = do_QueryInterface(pipeInputStream);
  mResponse->SetBody(cast);

  // Try to retarget off main thread.
  nsCOMPtr<nsIEventTarget> sts = do_GetService(NS_STREAMTRANSPORTSERVICE_CONTRACTID);
  if (sts) {
    nsCOMPtr<nsIThreadRetargetableRequest> rr = do_QueryInterface(aRequest);
    if (rr) {
      rr->RetargetDeliveryTo(sts);
    }
  }
  return NS_OK;
}

NS_IMETHODIMP
FetchDriver::OnDataAvailable(nsIRequest* aRequest,
                             nsISupports* aContext,
                             nsIInputStream* aInputStream,
                             uint64_t aOffset,
                             uint32_t aCount)
{
  uint32_t aRead;
  MOZ_ASSERT(mResponse);
  MOZ_ASSERT(mPipeOutputStream);

  nsresult rv = aInputStream->ReadSegments(NS_CopySegmentToStream,
                                           mPipeOutputStream,
                                           aCount, &aRead);
  return rv;
}

NS_IMETHODIMP
FetchDriver::OnStopRequest(nsIRequest* aRequest,
                           nsISupports* aContext,
                           nsresult aStatusCode)
{
  MOZ_ASSERT(mPipeOutputStream);
  mPipeOutputStream->Close();

  if (NS_FAILED(aStatusCode)) {
    return FailWithNetworkError();
  }

  ContinueHttpFetchAfterNetworkFetch();
  return NS_OK;
}

} // namespace dom
} // namespace mozilla
