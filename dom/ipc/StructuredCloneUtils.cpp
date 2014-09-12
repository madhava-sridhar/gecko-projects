/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*- */
/* vim: set sw=4 ts=8 et tw=80 : */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "StructuredCloneUtils.h"

#include "mozilla/dom/MessagePortBinding.h"
#include "nsIDOMFile.h"
#include "nsIDOMDOMException.h"
#include "nsIMutable.h"
#include "nsIXPConnect.h"

#include "nsContentUtils.h"
#include "nsHashKeys.h"
#include "nsJSEnvironment.h"
#include "nsTHashtable.h"
#include "MainThreadUtils.h"
#include "StructuredCloneTags.h"
#include "jsapi.h"


namespace mozilla {
namespace dom {

namespace {

struct
StructuredCloneClosureInternal
{
  StructuredCloneClosureInternal(StructuredCloneClosure& aClosure)
    : mClosure(aClosure)
  { }

  StructuredCloneClosure& mClosure;
  nsPIDOMWindow* mWindow;
  nsTArray<nsRefPtr<MessagePort>> mMessagePorts;
  nsTHashtable<nsRefPtrHashKey<MessagePortBase>> mTransferredPorts;
};

void
Error(JSContext* aCx, uint32_t aErrorId)
{
  MOZ_ASSERT(NS_IsMainThread());
  NS_DOMStructuredCloneError(aCx, aErrorId);
}

JSObject*
Read(JSContext* aCx, JSStructuredCloneReader* aReader, uint32_t aTag,
     uint32_t aData, void* aClosure)
{
  MOZ_ASSERT(NS_IsMainThread());
  MOZ_ASSERT(aClosure);

  StructuredCloneClosureInternal* closure =
    static_cast<StructuredCloneClosureInternal*>(aClosure);

  if (aTag == SCTAG_DOM_FILE) {
    MOZ_ASSERT(aData < closure->mClosure.mBlobs.Length());

    nsCOMPtr<nsIDOMFile> file = do_QueryInterface(closure->mClosure.mBlobs[aData]);
    MOZ_ASSERT(file);

#ifdef DEBUG
    {
      // File should not be mutable.
      nsCOMPtr<nsIMutable> mutableFile = do_QueryInterface(file);
      bool isMutable;
      MOZ_ASSERT(NS_SUCCEEDED(mutableFile->GetMutable(&isMutable)));
      MOZ_ASSERT(!isMutable);
    }
#endif

    JS::Rooted<JS::Value> wrappedFile(aCx);
    nsresult rv = nsContentUtils::WrapNative(aCx, file, &NS_GET_IID(nsIDOMFile),
                                             &wrappedFile);
    if (NS_FAILED(rv)) {
      Error(aCx, nsIDOMDOMException::DATA_CLONE_ERR);
      return nullptr;
    }

    return &wrappedFile.toObject();
  }

  if (aTag == SCTAG_DOM_BLOB) {
    MOZ_ASSERT(aData < closure->mClosure.mBlobs.Length());

    nsCOMPtr<nsIDOMBlob> blob = do_QueryInterface(closure->mClosure.mBlobs[aData]);
    MOZ_ASSERT(blob);

#ifdef DEBUG
    {
      // Blob should not be mutable.
      nsCOMPtr<nsIMutable> mutableBlob = do_QueryInterface(blob);
      bool isMutable;
      MOZ_ASSERT(NS_SUCCEEDED(mutableBlob->GetMutable(&isMutable)));
      MOZ_ASSERT(!isMutable);
    }
#endif

    JS::Rooted<JS::Value> wrappedBlob(aCx);
    nsresult rv = nsContentUtils::WrapNative(aCx, blob, &NS_GET_IID(nsIDOMBlob),
                                             &wrappedBlob);
    if (NS_FAILED(rv)) {
      Error(aCx, nsIDOMDOMException::DATA_CLONE_ERR);
      return nullptr;
    }

    return &wrappedBlob.toObject();
  }

  return NS_DOMReadStructuredClone(aCx, aReader, aTag, aData, nullptr);
}

bool
Write(JSContext* aCx, JSStructuredCloneWriter* aWriter,
      JS::Handle<JSObject*> aObj, void* aClosure)
{
  MOZ_ASSERT(NS_IsMainThread());
  MOZ_ASSERT(aClosure);

  StructuredCloneClosureInternal* closure =
    static_cast<StructuredCloneClosureInternal*>(aClosure);

  // See if this is a wrapped native.
  nsCOMPtr<nsIXPConnectWrappedNative> wrappedNative;
  nsContentUtils::XPConnect()->
    GetWrappedNativeOfJSObject(aCx, aObj, getter_AddRefs(wrappedNative));

  if (wrappedNative) {
    // Get the raw nsISupports out of it.
    nsISupports* wrappedObject = wrappedNative->Native();
    MOZ_ASSERT(wrappedObject);

    // See if the wrapped native is a nsIDOMFile.
    nsCOMPtr<nsIDOMFile> file = do_QueryInterface(wrappedObject);
    if (file) {
      nsCOMPtr<nsIMutable> mutableFile = do_QueryInterface(file);
      if (mutableFile &&
          NS_SUCCEEDED(mutableFile->SetMutable(false)) &&
          JS_WriteUint32Pair(aWriter, SCTAG_DOM_FILE,
                             closure->mClosure.mBlobs.Length())) {
        closure->mClosure.mBlobs.AppendElement(file);
        return true;
      }
    }

    // See if the wrapped native is a nsIDOMBlob.
    nsCOMPtr<nsIDOMBlob> blob = do_QueryInterface(wrappedObject);
    if (blob) {
      nsCOMPtr<nsIMutable> mutableBlob = do_QueryInterface(blob);
      if (mutableBlob &&
          NS_SUCCEEDED(mutableBlob->SetMutable(false)) &&
          JS_WriteUint32Pair(aWriter, SCTAG_DOM_BLOB,
                             closure->mClosure.mBlobs.Length())) {
        closure->mClosure.mBlobs.AppendElement(blob);
        return true;
      }
    }
  }

  return NS_DOMWriteStructuredClone(aCx, aWriter, aObj, nullptr);
}

bool
ReadTransfer(JSContext* aCx, JSStructuredCloneReader* aReader,
             uint32_t aTag, void* aContent, uint64_t aExtraData,
             void* aClosure, JS::MutableHandle<JSObject*> aReturnObject)
{
  MOZ_ASSERT(NS_IsMainThread());
  MOZ_ASSERT(aClosure);

  StructuredCloneClosureInternal* closure =
    static_cast<StructuredCloneClosureInternal*>(aClosure);

  if (aTag == SCTAG_DOM_MAP_MESSAGEPORT) {
    MOZ_ASSERT(aContent == 0);
    MOZ_ASSERT(aExtraData < closure->mClosure.mMessagePortIdentifiers.Length());

    nsRefPtr<MessagePort> port =
      new MessagePort(closure->mWindow,
                      closure->mClosure.mMessagePortIdentifiers[aExtraData]);
    closure->mMessagePorts.AppendElement(port);

    JS::Rooted<JSObject*> obj(aCx, port->WrapObject(aCx));
    if (!obj || !JS_WrapObject(aCx, &obj)) {
      return false;
    }

    aReturnObject.set(obj);
    return true;
  }

  return false;
}

bool
Transfer(JSContext* aCx, JS::Handle<JSObject*> aObj, void* aClosure,
         uint32_t* aTag, JS::TransferableOwnership* aOwnership,
         void** aContent, uint64_t *aExtraData)
{
  MOZ_ASSERT(NS_IsMainThread());
  MOZ_ASSERT(aClosure);

  StructuredCloneClosureInternal* closure =
    static_cast<StructuredCloneClosureInternal*>(aClosure);

  MessagePortBase *port = nullptr;
  nsresult rv = UNWRAP_OBJECT(MessagePort, aObj, port);
  if (NS_SUCCEEDED(rv)) {
    if (closure->mTransferredPorts.GetEntry(port)) {
      // No duplicate.
      return false;
    }

    MessagePortIdentifier* identifier =
      closure->mClosure.mMessagePortIdentifiers.AppendElement();

    if (!port->CloneAndDisentangle(*identifier)) {
      return false;
    }
    closure->mTransferredPorts.PutEntry(port);

    *aTag = SCTAG_DOM_MAP_MESSAGEPORT;
    *aOwnership = JS::SCTAG_TMO_CUSTOM;
    *aContent = nullptr;
    *aExtraData = closure->mClosure.mMessagePortIdentifiers.Length() - 1;

    return true;
  }

  return false;
}

void
FreeTransfer(uint32_t aTag, JS::TransferableOwnership aOwnership,
             void *aContent, uint64_t aExtraData, void* aClosure)
{
  // Nothing to do.
}

JSStructuredCloneCallbacks gCallbacks = {
  Read,
  Write,
  Error,
  ReadTransfer,
  Transfer,
  FreeTransfer
};

} // anonymous namespace

bool
ReadStructuredClone(JSContext* aCx, uint64_t* aData, size_t aDataLength,
                    const StructuredCloneClosure& aClosure,
                    JS::MutableHandle<JS::Value> aClone)
{
  MOZ_ASSERT(aClosure.mMessagePortIdentifiers.IsEmpty(),
             "You should use ReadStructuredCloneWithTransfer!");

  nsTArray<nsRefPtr<MessagePort>> transferable;
  bool rv = ReadStructuredCloneWithTransfer(aCx, aData, aDataLength, aClosure,
                                            aClone, nullptr, transferable);
  MOZ_ASSERT(transferable.IsEmpty());
  return rv;
}

bool
ReadStructuredCloneWithTransfer(JSContext* aCx, uint64_t* aData,
                                size_t aDataLength,
                                const StructuredCloneClosure& aClosure,
                                JS::MutableHandle<JS::Value> aClone,
                                nsPIDOMWindow* aParentWindow,
                                nsTArray<nsRefPtr<MessagePort>>& aMessagePorts)
{
  auto closure = const_cast<StructuredCloneClosure&>(aClosure);
  StructuredCloneClosureInternal internalClosure(closure);
  internalClosure.mWindow = aParentWindow;

  bool rv = !!JS_ReadStructuredClone(aCx, aData, aDataLength,
                                     JS_STRUCTURED_CLONE_VERSION, aClone,
                                     &gCallbacks, &internalClosure);
  if (rv) {
    aMessagePorts.SwapElements(internalClosure.mMessagePorts);
  }

  return rv;
}

bool
WriteStructuredClone(JSContext* aCx, JS::Handle<JS::Value> aSource,
                     JSAutoStructuredCloneBuffer& aBuffer,
                     StructuredCloneClosure& aClosure)
{
  JS::Rooted<JS::Value> transferable(aCx, JS::UndefinedValue());
  bool rv = WriteStructuredCloneWithTransfer(aCx, aSource, transferable,
                                             aBuffer, aClosure);
  MOZ_ASSERT(aClosure.mMessagePortIdentifiers.IsEmpty());
  return rv;
}

bool
WriteStructuredCloneWithTransfer(JSContext* aCx, JS::Handle<JS::Value> aSource,
                                 JS::Handle<JS::Value> aTransferable,
                                 JSAutoStructuredCloneBuffer& aBuffer,
                                 StructuredCloneClosure& aClosure)
{
  StructuredCloneClosureInternal internalClosure(aClosure);
  return aBuffer.write(aCx, aSource, aTransferable, &gCallbacks, &internalClosure);
}

} // namespace dom
} // namespace mozilla
