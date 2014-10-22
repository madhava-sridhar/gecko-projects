onfetch = function(ev) {
  if (ev.request.url.contains("synthesized.txt")) {
    var p = new Promise(function(resolve) {
      var r = new Response("synthesized response body", {});
      resolve(r);
    });
    ev.respondWith(p);
  }

  else if (ev.request.url.contains("ignored.txt")) {
  }

  else if (ev.request.url.contains("rejected.txt")) {
    var p = new Promise(function(resolve, reject) {
      reject();
    });
    ev.respondWith(p);
  }

  else if (ev.request.url.contains("nonresponse.txt")) {
    var p = new Promise(function(resolve, reject) {
      resolve(5);
    });
    ev.respondWith(p);
  }

  else if (ev.request.url.contains("nonresponse2.txt")) {
    var p = new Promise(function(resolve, reject) {
      resolve({});
    });
    ev.respondWith(p);
  }
}
