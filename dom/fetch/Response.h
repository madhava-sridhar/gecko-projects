/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_dom_Response_h
#define mozilla_dom_Response_h

#include "mozilla/dom/ResponseBinding.h"
#include "mozilla/dom/UnionTypes.h"
#include "ipc/IPCMessageUtils.h"

#include "InternalResponse.h"
#include "nsWrapperCache.h"
#include "nsISupportsImpl.h"

class nsPIDOMWindow;

namespace IPC {
  template<>
  struct ParamTraits<mozilla::dom::ResponseType> :
    public ContiguousTypedEnumSerializer<mozilla::dom::ResponseType,
                                         mozilla::dom::ResponseType::Basic,
                                         mozilla::dom::ResponseType::EndGuard_> {};
}

namespace mozilla {
namespace dom {

class Headers;

class Response MOZ_FINAL : public nsISupports
                         , public nsWrapperCache
{
  NS_DECL_CYCLE_COLLECTING_ISUPPORTS
  NS_DECL_CYCLE_COLLECTION_SCRIPT_HOLDER_CLASS(Response)

public:
  Response(nsISupports* aOwner);

  Response(nsIGlobalObject* aGlobal, InternalResponse* aInternalResponse);

  ResponseType
  Type() const
  {
    return mInternalResponse->Type();
  }

  void
  GetUrl(DOMString& aUrl) const
  {
    aUrl.AsAString() = NS_ConvertUTF8toUTF16(mInternalResponse->GetUrl());
  }

  uint16_t
  Status() const
  {
    return mInternalResponse->GetStatus();
  }

  void
  GetStatusText(nsCString& aStatusText) const
  {
    aStatusText = mInternalResponse->GetStatusText();
  }

  already_AddRefed<Headers>
  Headers_() const;

  static already_AddRefed<Response>
  Redirect(const GlobalObject& aGlobal, const nsAString& aUrl, uint16_t aStatus);

  static already_AddRefed<Response>
  Constructor(const GlobalObject& aGlobal,
              const Optional<ArrayBufferOrArrayBufferViewOrBlobOrFormDataOrScalarValueStringOrURLSearchParams>& aBody,
              const ResponseInit& aInit, ErrorResult& rv);

  virtual JSObject*
  WrapObject(JSContext* aCx)
  {
    return mozilla::dom::ResponseBinding::Wrap(aCx, this);
  }

  nsISupports* GetParentObject() const
  {
    return mOwner;
  }

  already_AddRefed<Promise>
  ArrayBuffer();

  already_AddRefed<Promise>
  Blob();

  already_AddRefed<Promise>
  FormData();

  already_AddRefed<Promise>
  Json();

  already_AddRefed<Promise>
  Text();

  bool
  BodyUsed();
private:
  ~Response();

  nsISupports* mOwner;
  nsRefPtr<InternalResponse> mInternalResponse;
  // nsRefPtr<FetchBodyStream> mBody;
};

} // namespace dom
} // namespace mozilla

#endif // mozilla_dom_Response_h
