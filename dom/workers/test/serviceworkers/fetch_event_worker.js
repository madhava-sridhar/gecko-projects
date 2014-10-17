onfetch = function(ev) {
  if (ev.request.url.contains("nonexistent.txt")) {
    var p = new Promise(function(resolve) {
      var r = new Response("synthesized response body", {});
      resolve(r);
    });
    ev.respondWith(p);
  }
}
