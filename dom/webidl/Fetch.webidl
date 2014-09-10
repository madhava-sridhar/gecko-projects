/* -*- Mode: IDL; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/.
 *
 * The origin of this IDL file is
 * http://fetch.spec.whatwg.org/
 */

typedef object JSON;
// FIXME: Bug 1025183 ScalarValueString.
typedef (ArrayBuffer or ArrayBufferView or Blob or FormData or ScalarValueString or URLSearchParams) BodyInit;

[NoInterfaceObject, Exposed=(Window,Worker)]
interface Body {
  readonly attribute boolean bodyUsed;
  Promise<ArrayBuffer> arrayBuffer();
  Promise<Blob> blob();
  Promise<FormData> formData();
  // Promise<JSON>
  Promise<object> json();
  Promise<ScalarValueString> text();
};

[NoInterfaceObject, Exposed=(Window,Worker)]
interface GlobalFetch {
  [Throws, Func="mozilla::dom::Headers::PrefEnabled"]
  Promise<Response> fetch(RequestInfo input, optional RequestInit init);
};

